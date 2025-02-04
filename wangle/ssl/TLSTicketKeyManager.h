/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <folly/io/async/EventBase.h>
#include <folly/io/async/SSLContext.h>
#include <folly/ssl/OpenSSLTicketHandler.h>

namespace wangle {

class SSLStats;
struct TLSTicketKeySeeds;
/**
 * The TLSTicketKeyManager handles TLS ticket key encryption and decryption in
 * a way that facilitates sharing the ticket keys across a range of servers.
 * Hash chaining is employed to achieve frequent key rotation with minimal
 * configuration change.  The scheme is as follows:
 *
 * The manager is supplied with three lists of seeds (old, current and new).
 * The config should be updated with new seeds periodically (e.g., daily).
 * 3 config changes are recommended to achieve the smoothest seed rotation
 * eg:
 *     1. Introduce new seed in the push prior to rotation
 *     2. Rotation push
 *     3. Remove old seeds in the push following rotation
 *
 * Multiple seeds are supported but only a single seed is required.
 *
 * Generating encryption keys from the seed works as follows.  For a given
 * seed, hash forward N times where N is currently the constant 1.
 * This is the base key.  The name of the base key is the first 4
 * bytes of hash(hash(seed), N).  This is copied into the first 4 bytes of the
 * TLS ticket key name field.
 *
 * For each new ticket encryption, the manager generates a random 12 byte salt.
 * Hash the salt and the base key together to form the encryption key for
 * that ticket.  The salt is included in the ticket's 'key name' field so it
 * can be used to derive the decryption key.  The salt is copied into the second
 * 8 bytes of the TLS ticket key name field.
 *
 * A key is valid for decryption for the lifetime of the instance.
 * Sessions will be valid for less time than that, which results in an extra
 * symmetric decryption to discover the session is expired.
 *
 * A TLSTicketKeyManager should be used in only one thread, and should have
 * a 1:1 relationship with the SSLContext provided.
 *
 */
class TLSTicketKeyManager : public folly::OpenSSLTicketHandler {
 public:
  static std::unique_ptr<TLSTicketKeyManager> fromSeeds(
      const TLSTicketKeySeeds* seeds);

  TLSTicketKeyManager();

  virtual ~TLSTicketKeyManager();

  int ticketCallback(
      SSL* ssl,
      unsigned char* keyName,
      unsigned char* iv,
      EVP_CIPHER_CTX* cipherCtx,
      HMAC_CTX* hmacCtx,
      int encrypt) override;

  /**
   * Initialize the manager with three sets of seeds.  There must be at least
   * one current seed, or the manager will revert to the default SSL behavior.
   *
   * @param oldSeeds Seeds previously used which can still decrypt.
   * @param currentSeeds Seeds to use for new ticket encryptions.
   * @param newSeeds Seeds which will be used soon, can be used to decrypt
   *                 in case some servers in the cluster have already rotated.
   */
  bool setTLSTicketKeySeeds(
      const std::vector<std::string>& oldSeeds,
      const std::vector<std::string>& currentSeeds,
      const std::vector<std::string>& newSeeds);

  bool getTLSTicketKeySeeds(
      std::vector<std::string>& oldSeeds,
      std::vector<std::string>& currentSeeds,
      std::vector<std::string>& newSeeds) const;

  /**
   * Stats object can record new tickets and ticket secret rotations.
   */
  void setStats(SSLStats* stats) {
    stats_ = stats;
  }

 private:
  TLSTicketKeyManager(const TLSTicketKeyManager&) = delete;
  TLSTicketKeyManager& operator=(const TLSTicketKeyManager&) = delete;

  int encryptCallback(
      unsigned char* keyName,
      unsigned char* iv,
      EVP_CIPHER_CTX* cipherCtx,
      HMAC_CTX* hmacCtx);

  int decryptCallback(
      unsigned char* keyName,
      unsigned char* iv,
      EVP_CIPHER_CTX* cipherCtx,
      HMAC_CTX* hmacCtx);

  enum TLSTicketSeedType { SEED_OLD = 0, SEED_CURRENT, SEED_NEW };

  /* The seeds supplied by the configuration */
  struct TLSTicketSeed {
    std::string seed_;
    TLSTicketSeedType type_;
    unsigned char seedName_[SHA256_DIGEST_LENGTH];
  };

  struct TLSTicketKeySource {
    int32_t hashCount_;
    std::string keyName_;
    TLSTicketSeedType type_;
    unsigned char keySource_[SHA256_DIGEST_LENGTH];
  };

  // Creates the name for the nth key generated from seed
  std::string
  makeKeyName(TLSTicketSeed* seed, uint32_t n, unsigned char* nameBuf);

  /**
   * Creates the key hashCount hashes from the given seed and inserts it in
   * ticketKeys.  A naked pointer to the key is returned for additional
   * processing if needed.
   */
  TLSTicketKeySource* insertNewKey(
      TLSTicketSeed* seed,
      uint32_t hashCount,
      TLSTicketKeySource* prevKeySource);

  /**
   * hashes input N times placing result in output, which must be at least
   * SHA256_DIGEST_LENGTH long.
   */
  void hashNth(
      const unsigned char* input,
      size_t input_len,
      unsigned char* output,
      uint32_t n);

  /**
   * Adds the given seed to the manager
   */
  TLSTicketSeed* insertSeed(
      const std::string& seedInput,
      TLSTicketSeedType type);

  /**
   * Locate a key for encrypting a new ticket
   */
  TLSTicketKeySource* findEncryptionKey();

  /**
   * Locate a key for decrypting a ticket with the given keyName
   */
  TLSTicketKeySource* findDecryptionKey(unsigned char* keyName);

  /**
   * Record the rotation of the ticket seeds with a new set
   */
  void recordTlsTicketRotation(
      const std::vector<std::string>& oldSeeds,
      const std::vector<std::string>& currentSeeds,
      const std::vector<std::string>& newSeeds);

  /**
   * Derive a unique key from the parent key and the salt via hashing
   */
  void makeUniqueKeys(
      unsigned char* parentKey,
      size_t keyLen,
      unsigned char* salt,
      unsigned char* output);

  typedef std::vector<std::unique_ptr<TLSTicketSeed>> TLSTicketSeedList;
  using TLSTicketKeyMap =
      std::map<std::string, std::unique_ptr<TLSTicketKeySource>>;
  using TLSActiveKeyList = std::vector<TLSTicketKeySource*>;

  TLSTicketSeedList ticketSeeds_;
  // All key sources that can be used for decryption
  TLSTicketKeyMap ticketKeys_;
  // Key sources that can be used for encryption
  TLSActiveKeyList activeKeys_;

  SSLStats* stats_{nullptr};
};
using TicketSeedHandler = TLSTicketKeyManager;
} // namespace wangle
