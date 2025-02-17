// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/crypto/impl/keyblob.h"

#include "sw/device/lib/base/hardened_memory.h"
#include "sw/device/lib/base/math.h"
#include "sw/device/lib/crypto/impl/integrity.h"
#include "sw/device/lib/crypto/impl/status.h"

// Module ID for status codes.
#define MODULE_ID MAKE_MODULE_ID('k', 'b', 'b')

enum {
  /**
   * Number of 32-bit words in a hardware-backed key's keyblob.
   */
  kHwBackedKeyblobWords = kKeymgrSaltNumWords,
  /**
   * Number of bytes in a hardware-backed key's keyblob.
   */
  kHwBackedKeyblobBytes = kHwBackedKeyblobWords * sizeof(uint32_t),
};

/**
 * Determine the number of bytes in one share of a blinded key.
 *
 * Normally, this is the same length as the unblinded key material. However, in
 * the case of some asymmetric keys, the shares might be longer.
 *
 * @param config Key configuration.
 * @return Number of bytes in one share of the blinded key.
 */
static size_t keyblob_share_num_bytes(const crypto_key_config_t config) {
  // Get the key type from the top 16 bits of the full mode.
  key_type_t key_type = (key_type_t)(config.key_mode >> 16);
  switch (launder32(key_type)) {
    case kKeyTypeEcc:
      // ECC keys have 64 extra redundant bits per share.
      HARDENED_CHECK_EQ(config.key_mode >> 16, kKeyTypeEcc);
      return config.key_length + (64 / 8);
    case kKeyTypeRsa:
      // RSA key shares are the same size as the unmasked key.
      // TODO: update once masking is implemented for RSA keys.
      HARDENED_CHECK_EQ(config.key_mode >> 16, kKeyTypeRsa);
      return config.key_length;
    default:
      // Symmetric key shares are simply the same size as the unmasked key.
      HARDENED_CHECK_NE(config.key_mode >> 16, kKeyTypeEcc);
      HARDENED_CHECK_NE(config.key_mode >> 16, kKeyTypeRsa);
      return config.key_length;
  }
  HARDENED_TRAP();
}

size_t keyblob_share_num_words(const crypto_key_config_t config) {
  size_t len_bytes = keyblob_share_num_bytes(config);
  return ceil_div(len_bytes, sizeof(uint32_t));
}

size_t keyblob_num_words(const crypto_key_config_t config) {
  if (launder32(config.hw_backed) == kHardenedBoolTrue) {
    HARDENED_CHECK_EQ(config.hw_backed, kHardenedBoolTrue);
    return kHwBackedKeyblobWords;
  }
  HARDENED_CHECK_NE(config.hw_backed, kHardenedBoolTrue);
  return 2 * keyblob_share_num_words(config);
}

/**
 * Check that the keyblob length matches expectations from the key config.
 *
 * Returns an OK status if the keyblob length is correct, a BAD_ARGS status
 * otherwise.
 *
 * @param key Blinded key.
 * @returns OK if the keyblob length is correct, BAD_ARGS otherwise.
 */
static status_t check_keyblob_length(const crypto_blinded_key_t *key) {
  size_t num_words = keyblob_num_words(key->config);
  if (launder32(key->keyblob_length) == num_words * sizeof(uint32_t)) {
    HARDENED_CHECK_EQ(key->keyblob_length, num_words * sizeof(uint32_t));
    HARDENED_CHECK_LE(key->keyblob_length / sizeof(uint32_t), num_words);
    return OTCRYPTO_OK;
  }
  return OTCRYPTO_BAD_ARGS;
}

status_t keyblob_to_shares(const crypto_blinded_key_t *key, uint32_t **share0,
                           uint32_t **share1) {
  // Double-check the length of the keyblob.
  HARDENED_TRY(check_keyblob_length(key));

  size_t key_words = keyblob_share_num_words(key->config);
  *share0 = key->keyblob;
  *share1 = &key->keyblob[key_words];
  return OTCRYPTO_OK;
}

void keyblob_from_shares(const uint32_t *share0, const uint32_t *share1,
                         const crypto_key_config_t config, uint32_t *keyblob) {
  size_t share_words = keyblob_share_num_words(config);
  hardened_memcpy(keyblob, share0, share_words);
  hardened_memcpy(keyblob + share_words, share1, share_words);
}

status_t keyblob_to_keymgr_diversification(
    const crypto_blinded_key_t *key,
    keymgr_diversification_t *diversification) {
  if (launder32(key->config.hw_backed) != kHardenedBoolTrue ||
      key->keyblob == NULL) {
    return OTCRYPTO_BAD_ARGS;
  }
  HARDENED_CHECK_EQ(key->config.hw_backed, kHardenedBoolTrue);

  if (key->keyblob_length != kHwBackedKeyblobBytes) {
    return OTCRYPTO_BAD_ARGS;
  }

  // Set the version to the first word of the keyblob.
  diversification->version = launder32(key->keyblob[0]);

  // Copy the remainder of the keyblob into the salt.
  hardened_memcpy(diversification->salt, &key->keyblob[1],
                  kKeymgrSaltNumWords - 1);

  // Set the key mode as the last word of the salt.
  diversification->salt[kKeymgrSaltNumWords - 1] =
      launder32(key->config.key_mode);

  HARDENED_CHECK_EQ(diversification->version, key->keyblob[0]);
  HARDENED_CHECK_EQ(hardened_memeq(diversification->salt, &key->keyblob[1],
                                   kKeymgrSaltNumWords - 1),
                    kHardenedBoolTrue);
  HARDENED_CHECK_EQ(diversification->salt[kKeymgrSaltNumWords - 1],
                    key->config.key_mode);
  return OTCRYPTO_OK;
}

status_t keyblob_ensure_xor_masked(const crypto_key_config_t config) {
  // Reject hardware-backed keys, since the keyblob is not the actual key
  // material in this case but the version/salt.
  if (launder32(config.hw_backed) != kHardenedBoolFalse) {
    return OTCRYPTO_BAD_ARGS;
  }
  HARDENED_CHECK_EQ(config.hw_backed, kHardenedBoolFalse);

  // Get the key type from the top 16 bits of the full mode.
  key_type_t key_type =
      (key_type_t)(launder32((uint32_t)config.key_mode) >> 16);
  int32_t result = (int32_t)launder32((uint32_t)(OTCRYPTO_OK.value ^ key_type));
  switch (launder32(key_type)) {
    case kKeyTypeAes:
      HARDENED_CHECK_EQ(config.key_mode >> 16, kKeyTypeAes);
      result ^= launder32(kKeyTypeAes);
      break;
    case kKeyTypeHmac:
      HARDENED_CHECK_EQ(config.key_mode >> 16, kKeyTypeHmac);
      result ^= launder32(kKeyTypeHmac);
      break;
    case kKeyTypeKmac:
      HARDENED_CHECK_EQ(config.key_mode >> 16, kKeyTypeKmac);
      result ^= launder32(kKeyTypeKmac);
      break;
    case kKeyTypeKdf:
      HARDENED_CHECK_EQ(config.key_mode >> 16, kKeyTypeKdf);
      result ^= launder32(kKeyTypeKdf);
      break;
    case kKeyTypeEcc:
      // Asymmetric!
      return OTCRYPTO_BAD_ARGS;
    case kKeyTypeRsa:
      // Asymmetric!
      return OTCRYPTO_BAD_ARGS;
    default:
      // Unrecognized key type.
      return OTCRYPTO_BAD_ARGS;
  }
  HARDENED_CHECK_NE(config.key_mode >> 16, kKeyTypeEcc);
  HARDENED_CHECK_NE(config.key_mode >> 16, kKeyTypeRsa);

  // If we get here, the result should be OTCRYPTO_OK.
  return (status_t){.value = result};
}

status_t keyblob_from_key_and_mask(const uint32_t *key, const uint32_t *mask,
                                   const crypto_key_config_t config,
                                   uint32_t *keyblob) {
  // Check that the key is masked with XOR.
  HARDENED_TRY(keyblob_ensure_xor_masked(config));

  // share0 = key ^ mask, share1 = mask
  size_t key_words = keyblob_share_num_words(config);
  uint32_t share0[key_words];
  size_t i = 0;
  for (; launder32(i) < key_words; i++) {
    share0[i] = key[i] ^ mask[i];
  }
  HARDENED_CHECK_EQ(i, key_words);

  keyblob_from_shares(share0, mask, config, keyblob);
  return OTCRYPTO_OK;
}

status_t keyblob_remask(crypto_blinded_key_t *key, const uint32_t *mask) {
  // Check that the key is masked with XOR.
  HARDENED_TRY(keyblob_ensure_xor_masked(key->config));

  // Double-check the length of the keyblob.
  HARDENED_TRY(check_keyblob_length(key));

  size_t key_share_words = keyblob_share_num_words(key->config);
  size_t keyblob_words = keyblob_num_words(key->config);

  // Construct a new keyblob by re-masking.
  size_t i = 0;
  for (; launder32(i) < keyblob_words; i++) {
    key->keyblob[i] ^= mask[i % key_share_words];
  }
  HARDENED_CHECK_EQ(i, keyblob_words);

  // Update the key checksum.
  key->checksum = integrity_blinded_checksum(key);
  return OTCRYPTO_OK;
}
