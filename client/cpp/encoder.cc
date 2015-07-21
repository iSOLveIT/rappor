// Copyright 2015 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cassert>  // assert
#include <stdio.h>
#include <stdarg.h>  // va_list, etc.

#include "encoder.h"

namespace rappor {

void log(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
  fprintf(stderr, "\n");
}

void PrintMd5(Md5Digest md5) {
  // GAH!  sizeof(md5) does NOT work.  Because that's a pointer.
  for (size_t i = 0; i < sizeof(Md5Digest); ++i) {
    //printf("[%d]\n", i);
    fprintf(stderr, "%02x", md5[i]);
  }
  fprintf(stderr, "\n");
}

void PrintSha256(Sha256Digest h) {
  // GAH!  sizeof(md5) does NOT work.  Because that's a pointer.
  for (size_t i = 0; i < sizeof(Sha256Digest); ++i) {
    //printf("[%d]\n", i);
    fprintf(stderr, "%02x", h[i]);
  }
  fprintf(stderr, "\n");
}

static int kMaxBits = sizeof(Bits) * 8;

//
// Encoder
//

Encoder::Encoder(const Params& params, const Deps& deps) 
    : params_(params),
      deps_(deps) {

  // Validity constraints:
  //
  // bits fit in an integral type uint64_t:
  //   num_bits < 64 (or sizeof(Bits) * 8)
  // md5 is long enough:
  //   128 > ( num_hashes * log2(num_bits) )
  // sha256 is long enough:
  //   256 > num_bits + (prob_f resolution * num_bits)

  //log("num_bits: %d", num_bits_);
  if (params_.num_bits > kMaxBits) {
    log("num_bits (%d) can't be bigger than rappor::Bits type: (%d)",
        params_.num_bits, kMaxBits);
    assert(false);
  }
}

Bits Encoder::MakeBloomFilter(const std::string& value) const {
  const int num_bits = params_.num_bits;
  const int num_hashes = params_.num_hashes;

  Bits bloom = 0;

  // First do hashing.
  Md5Digest md5;
  // 4 byte cohort + actual value
  std::string hash_input(4 + value.size(), '\0');

  // Assuming num_cohorts <= 256 , the big endian representation looks like
  // [0 0 0 <cohort>]
  unsigned char c = deps_.cohort_ & 0xFF;
  hash_input[0] = '\0';
  hash_input[1] = '\0';
  hash_input[2] = '\0';
  hash_input[3] = c;

  // Copy the rest
  for (size_t i = 0; i < value.size(); ++i) {
    hash_input[i + 4] = value[i];
  }

  deps_.md5_func_(hash_input, md5);

  log("MD5:");
  PrintMd5(md5);

  // To determine which bit to set in the bloom filter, use a byte of the MD5.
  for (int i = 0; i < num_hashes; ++i) {
    int bit_to_set = md5[i] % num_bits;
    bloom |= 1 << bit_to_set;
    //log("Hash %d, set bit %d", i, bit_to_set);
  }
  return bloom;
}

// Helper function for PRR
void Encoder::GetPrrMasks(const std::string& value, Bits* uniform_out,
                          Bits* f_mask_out) const {
  // Create HMAC(secret, value), and use its bits to construct f and uniform
  // bits.
  Sha256Digest sha256;
  deps_.hmac_func_(deps_.client_secret_, value, sha256);

  log("secret: %s word: %s sha256:", deps_.client_secret_.c_str(),
      value.c_str());
  PrintSha256(sha256);

  // We should have already checked this.
  assert(params_.num_bits <= 32);

  uint8_t threshold128 = static_cast<uint8_t>(params_.prob_f * 128);

  Bits uniform = 0;
  Bits f_mask = 0;

  for (int i = 0; i < params_.num_bits; ++i) {
    uint8_t byte = sha256[i];

    uint8_t u_bit = byte & 0x01;  // 1 bit of entropy
    uniform |= (u_bit << i);  // maybe set bit in mask

    uint8_t rand128 = byte >> 1;  // 7 bits of entropy
    uint8_t noise_bit = (rand128 < threshold128);
    f_mask |= (noise_bit << i);  // maybe set bit in mask
  }

  *uniform_out = uniform;
  *f_mask_out = f_mask;
}

bool Encoder::_EncodeInternal(const std::string& value, Bits* bloom_out,
    Bits* prr_out, Bits* irr_out) const {
  rappor::log("Encode '%s' cohort %d", value.c_str(), deps_.cohort_);

  Bits bloom = MakeBloomFilter(value);
  *bloom_out = bloom;

  // Compute Permanent Randomized Response (PRR).
  Bits uniform;
  Bits f_mask;
  GetPrrMasks(value, &uniform, &f_mask);

  Bits prr = (bloom & ~f_mask) | (uniform & f_mask);
  *prr_out = prr;

  // Compute Instantaneous Randomized Response (IRR).

  // NOTE: These can fail if say a read() from /dev/urandom fails.
  Bits p_bits;
  Bits q_bits;
  if (!deps_.irr_rand_.PMask(&p_bits)) {
    rappor::log("PMask failed");
    return false;
  }
  if (!deps_.irr_rand_.QMask(&q_bits)) {
    rappor::log("QMask failed");
    return false;
  }

  Bits irr = (p_bits & ~prr) | (q_bits & prr);
  *irr_out = irr;

  return true;
}

bool Encoder::Encode(const std::string& value, Bits* irr_out) const {
  Bits unused_bloom;
  Bits unused_prr;
  return _EncodeInternal(value, &unused_bloom, &unused_prr, irr_out);
}

}  // namespace rappor