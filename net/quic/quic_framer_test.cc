// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "base/hash_tables.h"
#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "base/port.h"
#include "base/stl_util.h"
#include "net/quic/quic_framer.h"
#include "net/quic/quic_protocol.h"
#include "net/quic/quic_utils.h"
#include "net/quic/test_tools/quic_test_utils.h"

using base::hash_set;
using base::StringPiece;
using std::make_pair;
using std::map;
using std::numeric_limits;
using std::string;
using std::vector;

namespace net {
namespace test {

const QuicPacketSequenceNumber kEpoch = GG_UINT64_C(1) << 48;
const QuicPacketSequenceNumber kMask = kEpoch - 1;

class QuicFramerPeer {
 public:
  static QuicPacketSequenceNumber CalculatePacketSequenceNumberFromWire(
      QuicFramer* framer,
      QuicPacketSequenceNumber packet_sequence_number) {
    return framer->CalculatePacketSequenceNumberFromWire(
        packet_sequence_number);
  }
  static void SetLastSequenceNumber(
      QuicFramer* framer,
      QuicPacketSequenceNumber packet_sequence_number) {
    framer->last_sequence_number_ = packet_sequence_number;
  }
};

class TestEncrypter : public QuicEncrypter {
 public:
  virtual ~TestEncrypter() {}
  virtual bool SetKey(StringPiece key) OVERRIDE {
    return true;
  }
  virtual bool SetNoncePrefix(StringPiece nonce_prefix) OVERRIDE {
    return true;
  }
  virtual QuicData* Encrypt(QuicPacketSequenceNumber sequence_number,
                            StringPiece associated_data,
                            StringPiece plaintext) OVERRIDE{
    sequence_number_ = sequence_number;
    associated_data_ = associated_data.as_string();
    plaintext_ = plaintext.as_string();
    return new QuicData(plaintext.data(), plaintext.length());
  }
  virtual size_t GetKeySize() const OVERRIDE {
    return 0;
  }
  virtual size_t GetNoncePrefixSize() const OVERRIDE{
    return 0;
  }
  virtual size_t GetMaxPlaintextSize(size_t ciphertext_size) const OVERRIDE {
    return ciphertext_size;
  }
  virtual size_t GetCiphertextSize(size_t plaintext_size) const OVERRIDE {
    return plaintext_size;
  }
  QuicPacketSequenceNumber sequence_number_;
  string associated_data_;
  string plaintext_;
};

class TestDecrypter : public QuicDecrypter {
 public:
  virtual ~TestDecrypter() {}
  virtual bool SetKey(StringPiece key) OVERRIDE {
    return true;
  }
  virtual bool SetNoncePrefix(StringPiece nonce_prefix) OVERRIDE {
    return true;
  }
  virtual QuicData* Decrypt(QuicPacketSequenceNumber sequence_number,
                            StringPiece associated_data,
                            StringPiece ciphertext) OVERRIDE{
    sequence_number_ = sequence_number;
    associated_data_ = associated_data.as_string();
    ciphertext_ = ciphertext.as_string();
    return new QuicData(ciphertext.data(), ciphertext.length());
  }
  QuicPacketSequenceNumber sequence_number_;
  string associated_data_;
  string ciphertext_;
};

class TestQuicVisitor : public ::net::QuicFramerVisitorInterface {
 public:
  TestQuicVisitor()
      : error_count_(0),
        packet_count_(0),
        frame_count_(0),
        fec_count_(0),
        complete_packets_(0),
        revived_packets_(0),
        accept_packet_(true) {
  }

  virtual ~TestQuicVisitor() {
    STLDeleteElements(&stream_frames_);
    STLDeleteElements(&ack_frames_);
    STLDeleteElements(&congestion_feedback_frames_);
    STLDeleteElements(&fec_data_);
  }

  virtual void OnError(QuicFramer* f) OVERRIDE {
    DLOG(INFO) << "QuicFramer Error: " << QuicUtils::ErrorToString(f->error())
               << " (" << f->error() << ")";
    error_count_++;
  }

  virtual void OnPacket() OVERRIDE {}

  virtual void OnPublicResetPacket(
      const QuicPublicResetPacket& packet) OVERRIDE {
    public_reset_packet_.reset(new QuicPublicResetPacket(packet));
  }

  virtual void OnRevivedPacket() OVERRIDE {
    revived_packets_++;
  }

  virtual bool OnPacketHeader(const QuicPacketHeader& header) OVERRIDE {
    packet_count_++;
    header_.reset(new QuicPacketHeader(header));
    return accept_packet_;
  }

  virtual void OnStreamFrame(const QuicStreamFrame& frame) OVERRIDE {
    frame_count_++;
    stream_frames_.push_back(new QuicStreamFrame(frame));
  }

  virtual void OnFecProtectedPayload(StringPiece payload) OVERRIDE {
    fec_protected_payload_ = payload.as_string();
  }

  virtual void OnAckFrame(const QuicAckFrame& frame) OVERRIDE {
    frame_count_++;
    ack_frames_.push_back(new QuicAckFrame(frame));
  }

  virtual void OnCongestionFeedbackFrame(
      const QuicCongestionFeedbackFrame& frame) OVERRIDE {
    frame_count_++;
    congestion_feedback_frames_.push_back(
        new QuicCongestionFeedbackFrame(frame));
  }

  virtual void OnFecData(const QuicFecData& fec) OVERRIDE {
    fec_count_++;
    fec_data_.push_back(new QuicFecData(fec));
  }

  virtual void OnPacketComplete() OVERRIDE {
    complete_packets_++;
  }

  virtual void OnRstStreamFrame(const QuicRstStreamFrame& frame) OVERRIDE {
    rst_stream_frame_ = frame;
  }

  virtual void OnConnectionCloseFrame(
      const QuicConnectionCloseFrame& frame) OVERRIDE {
    connection_close_frame_ = frame;
  }

  virtual void OnGoAwayFrame(const QuicGoAwayFrame& frame) {
    goaway_frame_ = frame;
  }

  // Counters from the visitor_ callbacks.
  int error_count_;
  int packet_count_;
  int frame_count_;
  int fec_count_;
  int complete_packets_;
  int revived_packets_;
  bool accept_packet_;

  scoped_ptr<QuicPacketHeader> header_;
  scoped_ptr<QuicPublicResetPacket> public_reset_packet_;
  vector<QuicStreamFrame*> stream_frames_;
  vector<QuicAckFrame*> ack_frames_;
  vector<QuicCongestionFeedbackFrame*> congestion_feedback_frames_;
  vector<QuicFecData*> fec_data_;
  string fec_protected_payload_;
  QuicRstStreamFrame rst_stream_frame_;
  QuicConnectionCloseFrame connection_close_frame_;
  QuicGoAwayFrame goaway_frame_;
};

class QuicFramerTest : public ::testing::Test {
 public:
  QuicFramerTest()
      : encrypter_(new test::TestEncrypter()),
        decrypter_(new test::TestDecrypter()),
        framer_(decrypter_, encrypter_) {
    framer_.set_visitor(&visitor_);
    framer_.set_entropy_calculator(&entropy_calculator_);
  }

  bool CheckEncryption(QuicPacketSequenceNumber sequence_number,
                       StringPiece packet) {
    if (sequence_number != encrypter_->sequence_number_) {
      LOG(ERROR) << "Encrypted incorrect packet sequence number.  expected "
                 << sequence_number << " actual: "
                 << encrypter_->sequence_number_;
      return false;
    }
    StringPiece associated_data(
        packet.substr(kStartOfHashData,
                      kStartOfEncryptedData - kStartOfHashData));
    if (associated_data != encrypter_->associated_data_) {
      LOG(ERROR) << "Encrypted incorrect associated data.  expected "
                 << associated_data << " actual: "
                 << encrypter_->associated_data_;
      return false;
    }
    StringPiece plaintext(packet.substr(kStartOfEncryptedData));
    if (plaintext != encrypter_->plaintext_) {
      LOG(ERROR) << "Encrypted incorrect plaintext data.  expected "
                 << plaintext << " actual: "
                 << encrypter_->plaintext_;
      return false;
    }
    return true;
  }

  bool CheckDecryption(StringPiece packet) {
    if (visitor_.header_->packet_sequence_number !=
        decrypter_->sequence_number_) {
      LOG(ERROR) << "Decrypted incorrect packet sequence number.  expected "
                 << visitor_.header_->packet_sequence_number << " actual: "
                 << decrypter_->sequence_number_;
      return false;
    }
    StringPiece associated_data(
        packet.substr(kStartOfHashData,
                      kStartOfEncryptedData - kStartOfHashData));
    if (associated_data != decrypter_->associated_data_) {
      LOG(ERROR) << "Decrypted incorrect associated data.  expected "
                 << associated_data << " actual: "
                 << decrypter_->associated_data_;
      return false;
    }
    StringPiece ciphertext(packet.substr(kStartOfEncryptedData));
    if (ciphertext != decrypter_->ciphertext_) {
      LOG(ERROR) << "Decrypted incorrect chipertext data.  expected "
                 << ciphertext << " actual: "
                 << decrypter_->ciphertext_;
      return false;
    }
    return true;
  }

  char* AsChars(unsigned char* data) {
    return reinterpret_cast<char*>(data);
  }

  void CheckProcessingFails(unsigned char* packet,
                            size_t len,
                            string expected_error,
                            QuicErrorCode error_code) {
    QuicEncryptedPacket encrypted(AsChars(packet), len, false);
    EXPECT_FALSE(framer_.ProcessPacket(encrypted)) << "len: " << len;
    EXPECT_EQ(expected_error, framer_.detailed_error()) << "len: " << len;
    EXPECT_EQ(error_code, framer_.error()) << "len: " << len;
  }

  void ValidateTruncatedAck(const QuicAckFrame* ack, size_t keys) {
    for (size_t i = 1; i < keys; ++i) {
      EXPECT_TRUE(ContainsKey(ack->received_info.missing_packets, i)) << i;
    }
    EXPECT_EQ(keys, ack->received_info.largest_observed);
  }

  void CheckCalculatePacketSequenceNumber(
      QuicPacketSequenceNumber expected_sequence_number,
      QuicPacketSequenceNumber last_sequence_number) {
    QuicPacketSequenceNumber wire_sequence_number =
        expected_sequence_number & kMask;
    QuicFramerPeer::SetLastSequenceNumber(&framer_, last_sequence_number);
    EXPECT_EQ(expected_sequence_number,
              QuicFramerPeer::CalculatePacketSequenceNumberFromWire(
                  &framer_, wire_sequence_number))
        << "last_sequence_number: " << last_sequence_number
        << "wire_sequence_number: " << wire_sequence_number;
  }

  test::TestEncrypter* encrypter_;
  test::TestDecrypter* decrypter_;
  QuicFramer framer_;
  test::TestQuicVisitor visitor_;
  test::TestEntropyCalculator entropy_calculator_;
};

TEST_F(QuicFramerTest, CalculatePacketSequenceNumberFromWireNearEpochStart) {
  // A few quick manual sanity checks
  CheckCalculatePacketSequenceNumber(GG_UINT64_C(1), GG_UINT64_C(0));
  CheckCalculatePacketSequenceNumber(kEpoch + 1, kMask);
  CheckCalculatePacketSequenceNumber(kEpoch, kMask);

  // Cases where the last number was close to the start of the range
  for (uint64 last = 0; last < 10; last++) {
    // Small numbers should not wrap (even if they're out of order).
    for (uint64 j = 0; j < 10; j++) {
      CheckCalculatePacketSequenceNumber(j, last);
    }

    // Large numbers should not wrap either (because we're near 0 already).
    for (uint64 j = 0; j < 10; j++) {
      CheckCalculatePacketSequenceNumber(kEpoch - 1 - j, last);
    }
  }
}

TEST_F(QuicFramerTest, CalculatePacketSequenceNumberFromWireNearEpochEnd) {
  // Cases where the last number was close to the end of the range
  for (uint64 i = 0; i < 10; i++) {
    QuicPacketSequenceNumber last = kEpoch - i;

    // Small numbers should wrap.
    for (uint64 j = 0; j < 10; j++) {
      CheckCalculatePacketSequenceNumber(kEpoch + j, last);
    }

    // Large numbers should not (even if they're out of order).
    for (uint64 j = 0; j < 10; j++) {
      CheckCalculatePacketSequenceNumber(kEpoch - 1 - j, last);
    }
  }
}

// Next check where we're in a non-zero epoch to verify we handle
// reverse wrapping, too.
TEST_F(QuicFramerTest, CalculatePacketSequenceNumberFromWireNearPrevEpoch) {
  const uint64 prev_epoch = 1 * kEpoch;
  const uint64 cur_epoch = 2 * kEpoch;
  // Cases where the last number was close to the start of the range
  for (uint64 i = 0; i < 10; i++) {
    uint64 last = cur_epoch + i;
    // Small number should not wrap (even if they're out of order).
    for (uint64 j = 0; j < 10; j++) {
      CheckCalculatePacketSequenceNumber(cur_epoch + j, last);
    }

    // But large numbers should reverse wrap.
    for (uint64 j = 0; j < 10; j++) {
      uint64 num = kEpoch - 1 - j;
      CheckCalculatePacketSequenceNumber(prev_epoch + num, last);
    }
  }
}

TEST_F(QuicFramerTest, CalculatePacketSequenceNumberFromWireNearNextEpoch) {
  const uint64 cur_epoch = 2 * kEpoch;
  const uint64 next_epoch = 3 * kEpoch;
  // Cases where the last number was close to the end of the range
  for (uint64 i = 0; i < 10; i++) {
    QuicPacketSequenceNumber last = next_epoch - 1 - i;

    // Small numbers should wrap.
    for (uint64 j = 0; j < 10; j++) {
      CheckCalculatePacketSequenceNumber(next_epoch + j, last);
    }

    // but large numbers should not (even if they're out of order).
    for (uint64 j = 0; j < 10; j++) {
      uint64 num = kEpoch - 1 - j;
      CheckCalculatePacketSequenceNumber(cur_epoch + num, last);
    }
  }
}

TEST_F(QuicFramerTest, CalculatePacketSequenceNumberFromWireNearNextMax) {
  const uint64 max_number = numeric_limits<uint64>::max();
  const uint64 max_epoch = max_number & ~kMask;

  // Cases where the last number was close to the end of the range
  for (uint64 i = 0; i < 10; i++) {
    QuicPacketSequenceNumber last = max_number - i;

    // Small numbers should not wrap (because they have nowhere to go.
    for (uint64 j = 0; j < 10; j++) {
      CheckCalculatePacketSequenceNumber(max_epoch + j, last);
    }

    // Large numbers should not wrap either.
    for (uint64 j = 0; j < 10; j++) {
      uint64 num = kEpoch - 1 - j;
      CheckCalculatePacketSequenceNumber(max_epoch + num, last);
    }
  }
}

TEST_F(QuicFramerTest, EmptyPacket) {
  char packet[] = { 0x00 };
  QuicEncryptedPacket encrypted(packet, 0, false);
  EXPECT_FALSE(framer_.ProcessPacket(encrypted));
  EXPECT_EQ(QUIC_INVALID_PACKET_HEADER, framer_.error());
}

TEST_F(QuicFramerTest, LargePacket) {
  unsigned char packet[kMaxPacketSize + 1] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags
    0x00,
    // first fec protected packet offset
    0xFF
  };

  memset(packet + kPacketHeaderSize, 0, kMaxPacketSize - kPacketHeaderSize + 1);

  QuicEncryptedPacket encrypted(AsChars(packet), arraysize(packet), false);
  EXPECT_FALSE(framer_.ProcessPacket(encrypted));

  ASSERT_TRUE(visitor_.header_.get());
  // Make sure we've parsed the packet header, so we can send an error.
  EXPECT_EQ(GG_UINT64_C(0xFEDCBA9876543210),
            visitor_.header_->public_header.guid);
  // Make sure the correct error is propagated.
  EXPECT_EQ(QUIC_PACKET_TOO_LARGE, framer_.error());
}

TEST_F(QuicFramerTest, PacketHeader) {
  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags
    0x00,
    // first fec protected packet offset
    0xFF,
  };

  QuicEncryptedPacket encrypted(AsChars(packet), arraysize(packet), false);
  EXPECT_FALSE(framer_.ProcessPacket(encrypted));
  EXPECT_EQ(QUIC_INVALID_FRAME_DATA, framer_.error());
  ASSERT_TRUE(visitor_.header_.get());
  EXPECT_EQ(GG_UINT64_C(0xFEDCBA9876543210),
            visitor_.header_->public_header.guid);
  EXPECT_FALSE(visitor_.header_->public_header.reset_flag);
  EXPECT_FALSE(visitor_.header_->public_header.version_flag);
  EXPECT_FALSE(visitor_.header_->fec_flag);
  EXPECT_FALSE(visitor_.header_->entropy_flag);
  EXPECT_FALSE(visitor_.header_->fec_entropy_flag);
  EXPECT_EQ(0, visitor_.header_->entropy_hash);
  EXPECT_EQ(GG_UINT64_C(0x123456789ABC),
            visitor_.header_->packet_sequence_number);
  EXPECT_EQ(0x00u, visitor_.header_->fec_group);

  // Now test framing boundaries
  for (size_t i = 0; i < kPacketHeaderSize; ++i) {
    string expected_error;
    if (i < kPublicFlagsOffset) {
      expected_error = "Unable to read GUID.";
    } else if (i < kSequenceNumberOffset) {
      expected_error = "Unable to read public flags.";
    } else if (i < kPrivateFlagsOffset) {
      expected_error = "Unable to read sequence number.";
    } else if (i < kFecGroupOffset) {
      expected_error = "Unable to read private flags.";
    } else {
      expected_error = "Unable to read first fec protected packet offset.";
    }
    CheckProcessingFails(packet, i, expected_error, QUIC_INVALID_PACKET_HEADER);
  }
}

TEST_F(QuicFramerTest, InvalidPublicFlag) {
  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x07,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags
    0x00,
    // first fec protected packet offset
    0xFF,

    // frame count
    0x01,
    // frame type (stream frame)
    0x01,
    // stream id
    0x04, 0x03, 0x02, 0x01,
    // fin
    0x01,
    // offset
    0x54, 0x76, 0x10, 0x32,
    0xDC, 0xFE, 0x98, 0xBA,
    // data length
    0x0c, 0x00,
    // data
    'h',  'e',  'l',  'l',
    'o',  ' ',  'w',  'o',
    'r',  'l',  'd',  '!',
  };
  CheckProcessingFails(packet,
                       arraysize(packet),
                       "Illegal public flags value.",
                       QUIC_INVALID_PACKET_HEADER);
};

TEST_F(QuicFramerTest, InvalidPrivateFlag) {
  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags
    0x08,
    // first fec protected packet offset
    0xFF,

    // frame count
    0x01,
    // frame type (stream frame)
    0x01,
    // stream id
    0x04, 0x03, 0x02, 0x01,
    // fin
    0x01,
    // offset
    0x54, 0x76, 0x10, 0x32,
    0xDC, 0xFE, 0x98, 0xBA,
    // data length
    0x0c, 0x00,
    // data
    'h',  'e',  'l',  'l',
    'o',  ' ',  'w',  'o',
    'r',  'l',  'd',  '!',
  };
  CheckProcessingFails(packet,
                       arraysize(packet),
                       "Illegal private flags value.",
                       QUIC_INVALID_PACKET_HEADER);
};

TEST_F(QuicFramerTest, PaddingFrame) {
  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags
    0x00,
    // first fec protected packet offset
    0xFF,

    // frame type (padding frame)
    0x00,
    // Ignored data (which in this case is a stream frame)
    0x01,
    0x04, 0x03, 0x02, 0x01,
    0x01,
    0x54, 0x76, 0x10, 0x32,
    0xDC, 0xFE, 0x98, 0xBA,
    0x0c, 0x00,
    'h',  'e',  'l',  'l',
    'o',  ' ',  'w',  'o',
    'r',  'l',  'd',  '!',
  };

  QuicEncryptedPacket encrypted(AsChars(packet), arraysize(packet), false);
  EXPECT_TRUE(framer_.ProcessPacket(encrypted));
  EXPECT_EQ(QUIC_NO_ERROR, framer_.error());
  ASSERT_TRUE(visitor_.header_.get());
  EXPECT_TRUE(CheckDecryption(StringPiece(AsChars(packet), arraysize(packet))));

  ASSERT_EQ(0u, visitor_.stream_frames_.size());
  EXPECT_EQ(0u, visitor_.ack_frames_.size());

  // Now test framing boundaries
  for (size_t i = 0; i < 1; ++i) {
    string expected_error;
    if (i < 1) {
      expected_error = "Unable to read frame type.";
    }
    CheckProcessingFails(packet, i + kPacketHeaderSize, expected_error,
                         QUIC_INVALID_FRAME_DATA);
  }
}

TEST_F(QuicFramerTest, StreamFrame) {
  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags
    0x00,
    // first fec protected packet offset
    0xFF,

    // frame type (stream frame)
    0x01,
    // stream id
    0x04, 0x03, 0x02, 0x01,
    // fin
    0x01,
    // offset
    0x54, 0x76, 0x10, 0x32,
    0xDC, 0xFE, 0x98, 0xBA,
    // data length
    0x0c, 0x00,
    // data
    'h',  'e',  'l',  'l',
    'o',  ' ',  'w',  'o',
    'r',  'l',  'd',  '!',
  };

  QuicEncryptedPacket encrypted(AsChars(packet), arraysize(packet), false);
  EXPECT_TRUE(framer_.ProcessPacket(encrypted));

  EXPECT_EQ(QUIC_NO_ERROR, framer_.error());
  ASSERT_TRUE(visitor_.header_.get());
  EXPECT_TRUE(CheckDecryption(StringPiece(AsChars(packet), arraysize(packet))));

  ASSERT_EQ(1u, visitor_.stream_frames_.size());
  EXPECT_EQ(0u, visitor_.ack_frames_.size());
  EXPECT_EQ(static_cast<uint64>(0x01020304),
            visitor_.stream_frames_[0]->stream_id);
  EXPECT_TRUE(visitor_.stream_frames_[0]->fin);
  EXPECT_EQ(GG_UINT64_C(0xBA98FEDC32107654),
            visitor_.stream_frames_[0]->offset);
  EXPECT_EQ("hello world!", visitor_.stream_frames_[0]->data);

  // Now test framing boundaries
  for (size_t i = 0; i < 28; ++i) {
    string expected_error;
    if (i < 1) {
      expected_error = "Unable to read frame type.";
    } else if (i < 5) {
      expected_error = "Unable to read stream_id.";
    } else if (i < 6) {
      expected_error = "Unable to read fin.";
    } else if (i < 14) {
      expected_error = "Unable to read offset.";
    } else if (i < 28) {
      expected_error = "Unable to read frame data.";
    }
    CheckProcessingFails(packet, i + kPacketHeaderSize, expected_error,
                         QUIC_INVALID_FRAME_DATA);
  }
}

TEST_F(QuicFramerTest, RejectPacket) {
  visitor_.accept_packet_ = false;

  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags
    0x00,
    // first fec protected packet offset
    0xFF,

    // frame type (stream frame)
    0x01,
    // stream id
    0x04, 0x03, 0x02, 0x01,
    // fin
    0x01,
    // offset
    0x54, 0x76, 0x10, 0x32,
    0xDC, 0xFE, 0x98, 0xBA,
    // data length
    0x0c, 0x00,
    // data
    'h',  'e',  'l',  'l',
    'o',  ' ',  'w',  'o',
    'r',  'l',  'd',  '!',
  };

  QuicEncryptedPacket encrypted(AsChars(packet), arraysize(packet), false);
  EXPECT_TRUE(framer_.ProcessPacket(encrypted));

  EXPECT_EQ(QUIC_NO_ERROR, framer_.error());
  ASSERT_TRUE(visitor_.header_.get());
  EXPECT_TRUE(CheckDecryption(StringPiece(AsChars(packet), arraysize(packet))));

  ASSERT_EQ(0u, visitor_.stream_frames_.size());
  EXPECT_EQ(0u, visitor_.ack_frames_.size());
}

TEST_F(QuicFramerTest, RevivedStreamFrame) {
  unsigned char payload[] = {
    // frame type (stream frame)
    0x01,
    // stream id
    0x04, 0x03, 0x02, 0x01,
    // fin
    0x01,
    // offset
    0x54, 0x76, 0x10, 0x32,
    0xDC, 0xFE, 0x98, 0xBA,
    // data length
    0x0c, 0x00,
    // data
    'h',  'e',  'l',  'l',
    'o',  ' ',  'w',  'o',
    'r',  'l',  'd',  '!',
  };

  QuicPacketHeader header;
  header.public_header.guid = GG_UINT64_C(0xFEDCBA9876543210);
  header.public_header.reset_flag = false;
  header.public_header.version_flag = false;
  header.fec_flag = true;
  header.entropy_flag = true;
  header.fec_entropy_flag = false;
  header.packet_sequence_number = GG_UINT64_C(0x123456789ABC);
  header.fec_group = 0;

  // Do not encrypt the payload because the revived payload is post-encryption.
  EXPECT_TRUE(framer_.ProcessRevivedPacket(&header,
                                           StringPiece(AsChars(payload),
                                                       arraysize(payload))));

  EXPECT_EQ(QUIC_NO_ERROR, framer_.error());
  ASSERT_EQ(1, visitor_.revived_packets_);
  ASSERT_TRUE(visitor_.header_.get());
  EXPECT_EQ(GG_UINT64_C(0xFEDCBA9876543210),
            visitor_.header_->public_header.guid);
  EXPECT_FALSE(visitor_.header_->public_header.reset_flag);
  EXPECT_FALSE(visitor_.header_->public_header.version_flag);
  EXPECT_TRUE(visitor_.header_->fec_flag);
  EXPECT_TRUE(visitor_.header_->entropy_flag);
  EXPECT_FALSE(visitor_.header_->fec_entropy_flag);
  EXPECT_EQ(1 << (header.packet_sequence_number % 8),
            visitor_.header_->entropy_hash);
  EXPECT_EQ(GG_UINT64_C(0x123456789ABC),
            visitor_.header_->packet_sequence_number);
  EXPECT_EQ(0x00u, visitor_.header_->fec_group);

  ASSERT_EQ(1u, visitor_.stream_frames_.size());
  EXPECT_EQ(0u, visitor_.ack_frames_.size());
  EXPECT_EQ(GG_UINT64_C(0x01020304), visitor_.stream_frames_[0]->stream_id);
  EXPECT_TRUE(visitor_.stream_frames_[0]->fin);
  EXPECT_EQ(GG_UINT64_C(0xBA98FEDC32107654),
            visitor_.stream_frames_[0]->offset);
  EXPECT_EQ("hello world!", visitor_.stream_frames_[0]->data);
}

TEST_F(QuicFramerTest, StreamFrameInFecGroup) {
  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x12, 0x34,
    // private flags
    0x00,
    // first fec protected packet offset
    0x02,

    // frame type (stream frame)
    0x01,
    // stream id
    0x04, 0x03, 0x02, 0x01,
    // fin
    0x01,
    // offset
    0x54, 0x76, 0x10, 0x32,
    0xDC, 0xFE, 0x98, 0xBA,
    // data length
    0x0c, 0x00,
    // data
    'h',  'e',  'l',  'l',
    'o',  ' ',  'w',  'o',
    'r',  'l',  'd',  '!',
  };

  QuicEncryptedPacket encrypted(AsChars(packet), arraysize(packet), false);
  EXPECT_TRUE(framer_.ProcessPacket(encrypted));

  EXPECT_EQ(QUIC_NO_ERROR, framer_.error());
  ASSERT_TRUE(visitor_.header_.get());
  EXPECT_TRUE(CheckDecryption(StringPiece(AsChars(packet), arraysize(packet))));
  EXPECT_EQ(GG_UINT64_C(0x341256789ABA),
            visitor_.header_->fec_group);
  EXPECT_EQ(string(AsChars(packet) + kStartOfFecProtectedData,
                   arraysize(packet) - kStartOfFecProtectedData),
            visitor_.fec_protected_payload_);

  ASSERT_EQ(1u, visitor_.stream_frames_.size());
  EXPECT_EQ(0u, visitor_.ack_frames_.size());
  EXPECT_EQ(GG_UINT64_C(0x01020304), visitor_.stream_frames_[0]->stream_id);
  EXPECT_TRUE(visitor_.stream_frames_[0]->fin);
  EXPECT_EQ(GG_UINT64_C(0xBA98FEDC32107654),
            visitor_.stream_frames_[0]->offset);
  EXPECT_EQ("hello world!", visitor_.stream_frames_[0]->data);
}

TEST_F(QuicFramerTest, AckFrame) {
  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags
    0x00,
    // first fec protected packet offset
    0xFF,

    // frame type (ack frame)
    0x02,
    // entropy hash of sent packets till least awaiting - 1.
    0xAB,
    // least packet sequence number awaiting an ack
    0xA0, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // entropy hash of all received packets.
    0xBA,
    // largest observed packet sequence number
    0xBF, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // num missing packets
    0x01,
    // missing packet
    0xBE, 0x9A, 0x78, 0x56,
    0x34, 0x12,
  };

  QuicEncryptedPacket encrypted(AsChars(packet), arraysize(packet), false);
  EXPECT_TRUE(framer_.ProcessPacket(encrypted));

  EXPECT_EQ(QUIC_NO_ERROR, framer_.error());
  ASSERT_TRUE(visitor_.header_.get());
  EXPECT_TRUE(CheckDecryption(StringPiece(AsChars(packet), arraysize(packet))));

  EXPECT_EQ(0u, visitor_.stream_frames_.size());
  ASSERT_EQ(1u, visitor_.ack_frames_.size());
  const QuicAckFrame& frame = *visitor_.ack_frames_[0];
  EXPECT_EQ(0xAB, frame.sent_info.entropy_hash);
  EXPECT_EQ(0xBA, frame.received_info.entropy_hash);
  EXPECT_EQ(GG_UINT64_C(0x0123456789ABF), frame.received_info.largest_observed);
  ASSERT_EQ(1u, frame.received_info.missing_packets.size());
  SequenceNumberSet::const_iterator missing_iter =
      frame.received_info.missing_packets.begin();
  EXPECT_EQ(GG_UINT64_C(0x0123456789ABE), *missing_iter);
  EXPECT_EQ(GG_UINT64_C(0x0123456789AA0), frame.sent_info.least_unacked);

  // Now test framing boundaries
  for (size_t i = 0; i < 22; ++i) {
    string expected_error;
    if (i < 1) {
      expected_error = "Unable to read frame type.";
    } else if (i < 2) {
      expected_error = "Unable to read entropy hash for sent packets.";
    } else if (i < 8) {
      expected_error = "Unable to read least unacked.";
    } else if (i < 9) {
      expected_error = "Unable to read entropy hash for received packets.";
    } else if (i < 15) {
      expected_error = "Unable to read largest observed.";
    } else if (i < 16) {
      expected_error = "Unable to read num missing packets.";
    } else {
      expected_error = "Unable to read sequence number in missing packets.";
    }
    CheckProcessingFails(packet, i + kPacketHeaderSize, expected_error,
                         QUIC_INVALID_FRAME_DATA);
  }
}

TEST_F(QuicFramerTest, CongestionFeedbackFrameTCP) {
  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags
    0x00,
    // first fec protected packet offset
    0xFF,

    // frame type (congestion feedback frame)
    0x03,
    // congestion feedback type (tcp)
    0x00,
    // ack_frame.feedback.tcp.accumulated_number_of_lost_packets
    0x01, 0x02,
    // ack_frame.feedback.tcp.receive_window
    0x03, 0x04,
  };

  QuicEncryptedPacket encrypted(AsChars(packet), arraysize(packet), false);
  EXPECT_TRUE(framer_.ProcessPacket(encrypted));

  EXPECT_EQ(QUIC_NO_ERROR, framer_.error());
  ASSERT_TRUE(visitor_.header_.get());
  EXPECT_TRUE(CheckDecryption(StringPiece(AsChars(packet), arraysize(packet))));

  EXPECT_EQ(0u, visitor_.stream_frames_.size());
  ASSERT_EQ(1u, visitor_.congestion_feedback_frames_.size());
  const QuicCongestionFeedbackFrame& frame =
      *visitor_.congestion_feedback_frames_[0];
  ASSERT_EQ(kTCP, frame.type);
  EXPECT_EQ(0x0201,
            frame.tcp.accumulated_number_of_lost_packets);
  EXPECT_EQ(0x4030u, frame.tcp.receive_window);

  // Now test framing boundaries
  for (size_t i = 0; i < 6; ++i) {
    string expected_error;
    if (i < 1) {
      expected_error = "Unable to read frame type.";
    } else if (i < 2) {
      expected_error = "Unable to read congestion feedback type.";
    } else if (i < 4) {
      expected_error = "Unable to read accumulated number of lost packets.";
    } else if (i < 6) {
      expected_error = "Unable to read receive window.";
    }
    CheckProcessingFails(packet, i + kPacketHeaderSize, expected_error,
                         QUIC_INVALID_FRAME_DATA);
  }
}

TEST_F(QuicFramerTest, CongestionFeedbackFrameInterArrival) {
  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags
    0x00,
    // first fec protected packet offset
    0xFF,

    // frame type (congestion feedback frame)
    0x03,
    // congestion feedback type (inter arrival)
    0x01,
    // accumulated_number_of_lost_packets
    0x02, 0x03,
    // num received packets
    0x03,
    // smallest ack sequence number
    0xBA, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // ack time
    0x87, 0x96, 0xA5, 0xB4,
    0xC3, 0xD2, 0xE1, 0x07,
    // sequence delta
    0x01, 0x00,
    // time delta
    0x01, 0x00, 0x00, 0x00,
    // sequence delta (skip one packet)
    0x03, 0x00,
    // time delta
    0x02, 0x00, 0x00, 0x00,
  };

  QuicEncryptedPacket encrypted(AsChars(packet), arraysize(packet), false);
  EXPECT_TRUE(framer_.ProcessPacket(encrypted));

  EXPECT_EQ(QUIC_NO_ERROR, framer_.error());
  ASSERT_TRUE(visitor_.header_.get());
  EXPECT_TRUE(CheckDecryption(StringPiece(AsChars(packet), arraysize(packet))));

  EXPECT_EQ(0u, visitor_.stream_frames_.size());
  ASSERT_EQ(1u, visitor_.congestion_feedback_frames_.size());
  const QuicCongestionFeedbackFrame& frame =
      *visitor_.congestion_feedback_frames_[0];
  ASSERT_EQ(kInterArrival, frame.type);
  EXPECT_EQ(0x0302, frame.inter_arrival.
            accumulated_number_of_lost_packets);
  ASSERT_EQ(3u, frame.inter_arrival.received_packet_times.size());
  TimeMap::const_iterator iter =
      frame.inter_arrival.received_packet_times.begin();
  EXPECT_EQ(GG_UINT64_C(0x0123456789ABA), iter->first);
  EXPECT_EQ(QuicTime::FromMicroseconds(GG_UINT64_C(0x07E1D2C3B4A59687)),
            iter->second);
  ++iter;
  EXPECT_EQ(GG_UINT64_C(0x0123456789ABB), iter->first);
  EXPECT_EQ(QuicTime::FromMicroseconds(GG_UINT64_C(0x07E1D2C3B4A59688)),
            iter->second);
  ++iter;
  EXPECT_EQ(GG_UINT64_C(0x0123456789ABD), iter->first);
  EXPECT_EQ(QuicTime::FromMicroseconds(GG_UINT64_C(0x07E1D2C3B4A59689)),
            iter->second);

  // Now test framing boundaries
  for (size_t i = 0; i < 31; ++i) {
    string expected_error;
    if (i < 1) {
      expected_error = "Unable to read frame type.";
    } else if (i < 2) {
      expected_error = "Unable to read congestion feedback type.";
    } else if (i < 4) {
      expected_error = "Unable to read accumulated number of lost packets.";
    } else if (i < 5) {
      expected_error = "Unable to read num received packets.";
    } else if (i < 11) {
      expected_error = "Unable to read smallest received.";
    } else if (i < 19) {
      expected_error = "Unable to read time received.";
    } else if (i < 21) {
      expected_error = "Unable to read sequence delta in received packets.";
    } else if (i < 25) {
      expected_error = "Unable to read time delta in received packets.";
    } else if (i < 27) {
      expected_error = "Unable to read sequence delta in received packets.";
    } else if (i < 31) {
      expected_error = "Unable to read time delta in received packets.";
    }
    CheckProcessingFails(packet, i + kPacketHeaderSize, expected_error,
                         QUIC_INVALID_FRAME_DATA);
  }
}

TEST_F(QuicFramerTest, CongestionFeedbackFrameFixRate) {
  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags
    0x00,
    // first fec protected packet offset
    0xFF,

    // frame type (congestion feedback frame)
    0x03,
    // congestion feedback type (fix rate)
    0x02,
    // bitrate_in_bytes_per_second;
    0x01, 0x02, 0x03, 0x04,
  };

  QuicEncryptedPacket encrypted(AsChars(packet), arraysize(packet), false);
  EXPECT_TRUE(framer_.ProcessPacket(encrypted));

  EXPECT_EQ(QUIC_NO_ERROR, framer_.error());
  ASSERT_TRUE(visitor_.header_.get());
  EXPECT_TRUE(CheckDecryption(StringPiece(AsChars(packet), arraysize(packet))));

  EXPECT_EQ(0u, visitor_.stream_frames_.size());
  ASSERT_EQ(1u, visitor_.congestion_feedback_frames_.size());
  const QuicCongestionFeedbackFrame& frame =
      *visitor_.congestion_feedback_frames_[0];
  ASSERT_EQ(kFixRate, frame.type);
  EXPECT_EQ(static_cast<uint32>(0x04030201),
            frame.fix_rate.bitrate.ToBytesPerSecond());

  // Now test framing boundaries
  for (size_t i = 0; i < 6; ++i) {
    string expected_error;
    if (i < 1) {
      expected_error = "Unable to read frame type.";
    } else if (i < 2) {
      expected_error = "Unable to read congestion feedback type.";
    } else if (i < 6) {
      expected_error = "Unable to read bitrate.";
    }
    CheckProcessingFails(packet, i + kPacketHeaderSize, expected_error,
                         QUIC_INVALID_FRAME_DATA);
  }
}


TEST_F(QuicFramerTest, CongestionFeedbackFrameInvalidFeedback) {
  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags
    0x00,
    // first fec protected packet offset
    0xFF,

    // frame type (congestion feedback frame)
    0x03,
    // congestion feedback type (invalid)
    0x03,
  };

  QuicEncryptedPacket encrypted(AsChars(packet), arraysize(packet), false);
  EXPECT_FALSE(framer_.ProcessPacket(encrypted));
  EXPECT_TRUE(CheckDecryption(StringPiece(AsChars(packet), arraysize(packet))));
  EXPECT_EQ(QUIC_INVALID_FRAME_DATA, framer_.error());
}

TEST_F(QuicFramerTest, RstStreamFrame) {
  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags
    0x00,
    // first fec protected packet offset
    0xFF,

    // frame type (rst stream frame)
    0x04,
    // stream id
    0x04, 0x03, 0x02, 0x01,
    // error code
    0x08, 0x07, 0x06, 0x05,

    // error details length
    0x0d, 0x00,
    // error details
    'b',  'e',  'c',  'a',
    'u',  's',  'e',  ' ',
    'I',  ' ',  'c',  'a',
    'n',
  };

  QuicEncryptedPacket encrypted(AsChars(packet), arraysize(packet), false);
  EXPECT_TRUE(framer_.ProcessPacket(encrypted));

  EXPECT_EQ(QUIC_NO_ERROR, framer_.error());
  ASSERT_TRUE(visitor_.header_.get());
  EXPECT_TRUE(CheckDecryption(StringPiece(AsChars(packet), arraysize(packet))));

  EXPECT_EQ(GG_UINT64_C(0x01020304), visitor_.rst_stream_frame_.stream_id);
  EXPECT_EQ(0x05060708, visitor_.rst_stream_frame_.error_code);
  EXPECT_EQ("because I can", visitor_.rst_stream_frame_.error_details);

  // Now test framing boundaries
  for (size_t i = 2; i < 24; ++i) {
    string expected_error;
    if (i < 5) {
      expected_error = "Unable to read stream_id.";
    } else if (i < 9) {
      expected_error = "Unable to read rst stream error code.";
    } else if (i < 24) {
      expected_error = "Unable to read rst stream error details.";
    }
    CheckProcessingFails(packet, i + kPacketHeaderSize, expected_error,
                         QUIC_INVALID_RST_STREAM_DATA);
  }
}

TEST_F(QuicFramerTest, ConnectionCloseFrame) {
  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags
    0x00,
    // first fec protected packet offset
    0xFF,

    // frame type (connection close frame)
    0x05,
    // error code
    0x08, 0x07, 0x06, 0x05,

    // error details length
    0x0d, 0x00,
    // error details
    'b',  'e',  'c',  'a',
    'u',  's',  'e',  ' ',
    'I',  ' ',  'c',  'a',
    'n',

    // Ack frame.
    // entropy hash of sent packets till least awaiting - 1.
    0xBF,
    // least packet sequence number awaiting an ack
    0xA0, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // entropy hash of all received packets.
    0xEB,
    // largest observed packet sequence number
    0xBF, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // num missing packets
    0x01,
    // missing packet
    0xBE, 0x9A, 0x78, 0x56,
    0x34, 0x12,
  };

  QuicEncryptedPacket encrypted(AsChars(packet), arraysize(packet), false);
  EXPECT_TRUE(framer_.ProcessPacket(encrypted));

  EXPECT_EQ(QUIC_NO_ERROR, framer_.error());
  ASSERT_TRUE(visitor_.header_.get());
  EXPECT_TRUE(CheckDecryption(StringPiece(AsChars(packet), arraysize(packet))));

  EXPECT_EQ(0u, visitor_.stream_frames_.size());

  EXPECT_EQ(0x05060708, visitor_.connection_close_frame_.error_code);
  EXPECT_EQ("because I can", visitor_.connection_close_frame_.error_details);

  ASSERT_EQ(1u, visitor_.ack_frames_.size());
  const QuicAckFrame& frame = *visitor_.ack_frames_[0];
  EXPECT_EQ(0xBF, frame.sent_info.entropy_hash);
  EXPECT_EQ(GG_UINT64_C(0x0123456789AA0), frame.sent_info.least_unacked);
  EXPECT_EQ(0xEB, frame.received_info.entropy_hash);
  EXPECT_EQ(GG_UINT64_C(0x0123456789ABF), frame.received_info.largest_observed);
  ASSERT_EQ(1u, frame.received_info.missing_packets.size());
  SequenceNumberSet::const_iterator missing_iter =
      frame.received_info.missing_packets.begin();
  EXPECT_EQ(GG_UINT64_C(0x0123456789ABE), *missing_iter);

  // Now test framing boundaries
  for (size_t i = 2; i < 20; ++i) {
    string expected_error;
    if (i < 5) {
      expected_error = "Unable to read connection close error code.";
    } else if (i < 20) {
      expected_error = "Unable to read connection close error details.";
    }
    CheckProcessingFails(packet, i + kPacketHeaderSize, expected_error,
                         QUIC_INVALID_CONNECTION_CLOSE_DATA);
  }
}

TEST_F(QuicFramerTest, GoAwayFrame) {
  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags
    0x00,
    // first fec protected packet offset
    0xFF,

    // frame type (go away frame)
    0x06,
    // error code
    0x08, 0x07, 0x06, 0x05,
    // stream id
    0x04, 0x03, 0x02, 0x01,
    // error details length
    0x0d, 0x00,
    // error details
    'b',  'e',  'c',  'a',
    'u',  's',  'e',  ' ',
    'I',  ' ',  'c',  'a',
    'n',
  };

  QuicEncryptedPacket encrypted(AsChars(packet), arraysize(packet), false);
  EXPECT_TRUE(framer_.ProcessPacket(encrypted));

  EXPECT_EQ(QUIC_NO_ERROR, framer_.error());
  ASSERT_TRUE(visitor_.header_.get());
  EXPECT_TRUE(CheckDecryption(StringPiece(AsChars(packet), arraysize(packet))));

  EXPECT_EQ(GG_UINT64_C(0x01020304),
            visitor_.goaway_frame_.last_good_stream_id);
  EXPECT_EQ(0x05060708, visitor_.goaway_frame_.error_code);
  EXPECT_EQ("because I can", visitor_.goaway_frame_.reason_phrase);

  // Now test framing boundaries
  for (size_t i = 2; i < 24; ++i) {
    string expected_error;
    if (i < 5) {
      expected_error = "Unable to read go away error code.";
    } else if (i < 9) {
      expected_error = "Unable to read last good stream id.";
    } else if (i < 24) {
      expected_error = "Unable to read goaway reason.";
    }
    CheckProcessingFails(packet, i + kPacketHeaderSize, expected_error,
                         QUIC_INVALID_GOAWAY_DATA);
  }
}

TEST_F(QuicFramerTest, PublicResetPacket) {
  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags (public reset)
    0x02,
    // nonce proof
    0x89, 0x67, 0x45, 0x23,
    0x01, 0xEF, 0xCD, 0xAB,
    // rejected sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
  };

  QuicEncryptedPacket encrypted(AsChars(packet), arraysize(packet), false);
  EXPECT_TRUE(framer_.ProcessPacket(encrypted));
  ASSERT_EQ(QUIC_NO_ERROR, framer_.error());
  ASSERT_TRUE(visitor_.public_reset_packet_.get());
  EXPECT_EQ(GG_UINT64_C(0xFEDCBA9876543210),
            visitor_.public_reset_packet_->public_header.guid);
  EXPECT_TRUE(visitor_.public_reset_packet_->public_header.reset_flag);
  EXPECT_FALSE(visitor_.public_reset_packet_->public_header.version_flag);
  EXPECT_EQ(GG_UINT64_C(0xABCDEF0123456789),
            visitor_.public_reset_packet_->nonce_proof);
  EXPECT_EQ(GG_UINT64_C(0x123456789ABC),
            visitor_.public_reset_packet_->rejected_sequence_number);

  // Now test framing boundaries
  for (size_t i = 0; i < kPublicResetPacketSize; ++i) {
    string expected_error;
    if (i < kPublicFlagsOffset) {
      expected_error = "Unable to read GUID.";
    } else if (i < kPublicResetPacketNonceProofOffset) {
      expected_error = "Unable to read public flags.";
    } else if (i < kPublicResetPacketRejectedSequenceNumberOffset) {
      expected_error = "Unable to read nonce proof.";
    } else {
      expected_error = "Unable to read rejected sequence number.";
    }
    CheckProcessingFails(packet, i, expected_error, QUIC_INVALID_PACKET_HEADER);
  }
}

TEST_F(QuicFramerTest, FecPacket) {
  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags (FEC)
    0x01,
    // first fec protected packet offset
    0x01,

    // redundancy
    'a',  'b',  'c',  'd',
    'e',  'f',  'g',  'h',
    'i',  'j',  'k',  'l',
    'm',  'n',  'o',  'p',
  };

  QuicEncryptedPacket encrypted(AsChars(packet), arraysize(packet), false);
  EXPECT_TRUE(framer_.ProcessPacket(encrypted));

  EXPECT_EQ(QUIC_NO_ERROR, framer_.error());
  ASSERT_TRUE(visitor_.header_.get());
  EXPECT_TRUE(CheckDecryption(StringPiece(AsChars(packet), arraysize(packet))));

  EXPECT_EQ(0u, visitor_.stream_frames_.size());
  EXPECT_EQ(0u, visitor_.ack_frames_.size());
  ASSERT_EQ(1, visitor_.fec_count_);
  const QuicFecData& fec_data = *visitor_.fec_data_[0];
  EXPECT_EQ(GG_UINT64_C(0x0123456789ABB), fec_data.fec_group);
  EXPECT_EQ("abcdefghijklmnop", fec_data.redundancy);
}

TEST_F(QuicFramerTest, ConstructPaddingFramePacket) {
  QuicPacketHeader header;
  header.public_header.guid = GG_UINT64_C(0xFEDCBA9876543210);
  header.public_header.reset_flag = false;
  header.public_header.version_flag = false;
  header.fec_flag = false;
  header.entropy_flag = false;
  header.fec_entropy_flag = false;
  header.packet_sequence_number = GG_UINT64_C(0x123456789ABC);
  header.fec_group = 0;

  QuicPaddingFrame padding_frame;

  QuicFrames frames;
  frames.push_back(QuicFrame(&padding_frame));

  unsigned char packet[kMaxPacketSize] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags
    0x00,
    // first fec protected packet offset
    0xFF,

    // frame type (padding frame)
    0x00,
  };

  //  unsigned char packet[kMaxPacketSize];
  memset(packet + kPacketHeaderSize + 1, 0x00,
         kMaxPacketSize - kPacketHeaderSize - 1);

  scoped_ptr<QuicPacket> data(
      framer_.ConstructFrameDataPacket(header, frames).packet);
  ASSERT_TRUE(data != NULL);

  test::CompareCharArraysWithHexError("constructed packet",
                                      data->data(), data->length(),
                                      AsChars(packet), arraysize(packet));
}

TEST_F(QuicFramerTest, ConstructStreamFramePacket) {
  QuicPacketHeader header;
  header.public_header.guid = GG_UINT64_C(0xFEDCBA9876543210);
  header.public_header.reset_flag = false;
  header.public_header.version_flag = false;
  header.fec_flag = false;
  header.entropy_flag = true;
  header.fec_entropy_flag = false;
  header.packet_sequence_number = GG_UINT64_C(0x77123456789ABC);
  header.fec_group = 0;

  QuicStreamFrame stream_frame;
  stream_frame.stream_id = 0x01020304;
  stream_frame.fin = true;
  stream_frame.offset = GG_UINT64_C(0xBA98FEDC32107654);
  stream_frame.data = "hello world!";

  QuicFrames frames;
  frames.push_back(QuicFrame(&stream_frame));

  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags (entropy)
    0x02,
    // first fec protected packet offset
    0xFF,

    // frame type (stream frame)
    0x01,
    // stream id
    0x04, 0x03, 0x02, 0x01,
    // fin
    0x01,
    // offset
    0x54, 0x76, 0x10, 0x32,
    0xDC, 0xFE, 0x98, 0xBA,
    // data length
    0x0c, 0x00,
    // data
    'h',  'e',  'l',  'l',
    'o',  ' ',  'w',  'o',
    'r',  'l',  'd',  '!',
  };

  scoped_ptr<QuicPacket> data(
      framer_.ConstructFrameDataPacket(header, frames).packet);
  ASSERT_TRUE(data != NULL);

  test::CompareCharArraysWithHexError("constructed packet",
                                      data->data(), data->length(),
                                      AsChars(packet), arraysize(packet));
}

TEST_F(QuicFramerTest, ConstructAckFramePacket) {
  QuicPacketHeader header;
  header.public_header.guid = GG_UINT64_C(0xFEDCBA9876543210);
  header.public_header.reset_flag = false;
  header.public_header.version_flag = false;
  header.fec_flag = false;
  header.entropy_flag = true;
  header.fec_entropy_flag = true;
  header.packet_sequence_number = GG_UINT64_C(0x123456789ABC);
  header.fec_group = 0;

  QuicAckFrame ack_frame;
  ack_frame.received_info.entropy_hash = 0x43;
  ack_frame.received_info.largest_observed = GG_UINT64_C(0x770123456789ABF);
  ack_frame.received_info.missing_packets.insert(
      GG_UINT64_C(0x770123456789ABE));
  ack_frame.sent_info.entropy_hash = 0x14;
  ack_frame.sent_info.least_unacked = GG_UINT64_C(0x770123456789AA0);

  QuicFrames frames;
  frames.push_back(QuicFrame(&ack_frame));

  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags (entropy & fec_entropy -- not relevant)
    0x06,
    // first fec protected packet offset
    0xFF,

    // frame type (ack frame)
    0x02,
    // entropy hash of sent packets till least awaiting - 1.
    0x14,
    // least packet sequence number awaiting an ack
    0xA0, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // entropy hash of all received packets.
    0x43,
    // largest observed packet sequence number
    0xBF, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // num missing packets
    0x01,
    // missing packet
    0xBE, 0x9A, 0x78, 0x56,
    0x34, 0x12,
  };

  scoped_ptr<QuicPacket> data(
      framer_.ConstructFrameDataPacket(header, frames).packet);
  ASSERT_TRUE(data != NULL);

  test::CompareCharArraysWithHexError("constructed packet",
                                      data->data(), data->length(),
                                      AsChars(packet), arraysize(packet));
}

TEST_F(QuicFramerTest, ConstructCongestionFeedbackFramePacketTCP) {
  QuicPacketHeader header;
  header.public_header.guid = GG_UINT64_C(0xFEDCBA9876543210);
  header.public_header.reset_flag = false;
  header.public_header.version_flag = false;
  header.fec_flag = false;
  header.entropy_flag = false;
  header.fec_entropy_flag = true;
  header.packet_sequence_number = GG_UINT64_C(0x123456789ABC);
  header.fec_group = 0;

  QuicCongestionFeedbackFrame congestion_feedback_frame;
  congestion_feedback_frame.type = kTCP;
  congestion_feedback_frame.tcp.accumulated_number_of_lost_packets = 0x0201;
  congestion_feedback_frame.tcp.receive_window = 0x4030;

  QuicFrames frames;
  frames.push_back(QuicFrame(&congestion_feedback_frame));

  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags
    0x04,  // (fec_entropy_flag)
    // first fec protected packet offset
    0xFF,

    // frame type (congestion feedback frame)
    0x03,
    // congestion feedback type (TCP)
    0x00,
    // accumulated number of lost packets
    0x01, 0x02,
    // TCP receive window
    0x03, 0x04,
  };

  scoped_ptr<QuicPacket> data(
      framer_.ConstructFrameDataPacket(header, frames).packet);
  ASSERT_TRUE(data != NULL);

  test::CompareCharArraysWithHexError("constructed packet",
                                      data->data(), data->length(),
                                      AsChars(packet), arraysize(packet));
}

TEST_F(QuicFramerTest, ConstructCongestionFeedbackFramePacketInterArrival) {
  QuicPacketHeader header;
  header.public_header.guid = GG_UINT64_C(0xFEDCBA9876543210);
  header.public_header.reset_flag = false;
  header.public_header.version_flag = false;
  header.fec_flag = false;
  header.entropy_flag = false;
  header.fec_entropy_flag = false;
  header.packet_sequence_number = GG_UINT64_C(0x123456789ABC);
  header.fec_group = 0;

  QuicCongestionFeedbackFrame frame;
  frame.type = kInterArrival;
  frame.inter_arrival.accumulated_number_of_lost_packets = 0x0302;
  frame.inter_arrival.received_packet_times.insert(
      make_pair(GG_UINT64_C(0x0123456789ABA),
                QuicTime::FromMicroseconds(GG_UINT64_C(0x07E1D2C3B4A59687))));
  frame.inter_arrival.received_packet_times.insert(
      make_pair(GG_UINT64_C(0x0123456789ABB),
                QuicTime::FromMicroseconds(GG_UINT64_C(0x07E1D2C3B4A59688))));
  frame.inter_arrival.received_packet_times.insert(
      make_pair(GG_UINT64_C(0x0123456789ABD),
                QuicTime::FromMicroseconds(GG_UINT64_C(0x07E1D2C3B4A59689))));
  QuicFrames frames;
  frames.push_back(QuicFrame(&frame));

  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags
    0x00,
    // first fec protected packet offset
    0xFF,

    // frame type (congestion feedback frame)
    0x03,
    // congestion feedback type (inter arrival)
    0x01,
    // accumulated_number_of_lost_packets
    0x02, 0x03,
    // num received packets
    0x03,
    // smallest ack sequence number
    0xBA, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // ack time
    0x87, 0x96, 0xA5, 0xB4,
    0xC3, 0xD2, 0xE1, 0x07,
    // sequence delta
    0x01, 0x00,
    // time delta
    0x01, 0x00, 0x00, 0x00,
    // sequence delta (skip one packet)
    0x03, 0x00,
    // time delta
    0x02, 0x00, 0x00, 0x00,
  };

  scoped_ptr<QuicPacket> data(
      framer_.ConstructFrameDataPacket(header, frames).packet);
  ASSERT_TRUE(data != NULL);

  test::CompareCharArraysWithHexError("constructed packet",
                                      data->data(), data->length(),
                                      AsChars(packet), arraysize(packet));
}

TEST_F(QuicFramerTest, ConstructCongestionFeedbackFramePacketFixRate) {
  QuicPacketHeader header;
  header.public_header.guid = GG_UINT64_C(0xFEDCBA9876543210);
  header.public_header.reset_flag = false;
  header.public_header.version_flag = false;
  header.fec_flag = false;
  header.entropy_flag = false;
  header.fec_entropy_flag = false;
  header.packet_sequence_number = GG_UINT64_C(0x123456789ABC);
  header.fec_group = 0;

  QuicCongestionFeedbackFrame congestion_feedback_frame;
  congestion_feedback_frame.type = kFixRate;
  congestion_feedback_frame.fix_rate.bitrate
      = QuicBandwidth::FromBytesPerSecond(0x04030201);

  QuicFrames frames;
  frames.push_back(QuicFrame(&congestion_feedback_frame));

  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags
    0x00,
    // first fec protected packet offset
    0xFF,

    // frame type (congestion feedback frame)
    0x03,
    // congestion feedback type (fix rate)
    0x02,
    // bitrate_in_bytes_per_second;
    0x01, 0x02, 0x03, 0x04,
  };

  scoped_ptr<QuicPacket> data(
      framer_.ConstructFrameDataPacket(header, frames).packet);
  ASSERT_TRUE(data != NULL);

  test::CompareCharArraysWithHexError("constructed packet",
                                      data->data(), data->length(),
                                      AsChars(packet), arraysize(packet));
}

TEST_F(QuicFramerTest, ConstructCongestionFeedbackFramePacketInvalidFeedback) {
  QuicPacketHeader header;
  header.public_header.guid = GG_UINT64_C(0xFEDCBA9876543210);
  header.public_header.reset_flag = false;
  header.public_header.version_flag = false;
  header.fec_flag = false;
  header.entropy_flag = false;
  header.fec_entropy_flag = false;
  header.packet_sequence_number = GG_UINT64_C(0x123456789ABC);
  header.fec_group = 0;

  QuicCongestionFeedbackFrame congestion_feedback_frame;
  congestion_feedback_frame.type =
      static_cast<CongestionFeedbackType>(kFixRate + 1);

  QuicFrames frames;
  frames.push_back(QuicFrame(&congestion_feedback_frame));

  scoped_ptr<QuicPacket> data(
      framer_.ConstructFrameDataPacket(header, frames).packet);
  ASSERT_TRUE(data == NULL);
}

TEST_F(QuicFramerTest, ConstructRstFramePacket) {
  QuicPacketHeader header;
  header.public_header.guid = GG_UINT64_C(0xFEDCBA9876543210);
  header.public_header.reset_flag = false;
  header.public_header.version_flag = false;
  header.fec_flag = false;
  header.entropy_flag = false;
  header.fec_entropy_flag = false;
  header.packet_sequence_number = GG_UINT64_C(0x123456789ABC);
  header.fec_group = 0;

  QuicRstStreamFrame rst_frame;
  rst_frame.stream_id = 0x01020304;
  rst_frame.error_code = static_cast<QuicErrorCode>(0x05060708);
  rst_frame.error_details = "because I can";

  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags
    0x00,
    // first fec protected packet offset
    0xFF,

    // frame type (rst stream frame)
    0x04,
    // stream id
    0x04, 0x03, 0x02, 0x01,
    // error code
    0x08, 0x07, 0x06, 0x05,
    // error details length
    0x0d, 0x00,
    // error details
    'b',  'e',  'c',  'a',
    'u',  's',  'e',  ' ',
    'I',  ' ',  'c',  'a',
    'n',
  };

  QuicFrames frames;
  frames.push_back(QuicFrame(&rst_frame));

  scoped_ptr<QuicPacket> data(
      framer_.ConstructFrameDataPacket(header, frames).packet);
  ASSERT_TRUE(data != NULL);

  test::CompareCharArraysWithHexError("constructed packet",
                                      data->data(), data->length(),
                                      AsChars(packet), arraysize(packet));
}

TEST_F(QuicFramerTest, ConstructCloseFramePacket) {
  QuicPacketHeader header;
  header.public_header.guid = GG_UINT64_C(0xFEDCBA9876543210);
  header.public_header.reset_flag = false;
  header.public_header.version_flag = false;
  header.fec_flag = false;
  header.entropy_flag = true;
  header.fec_entropy_flag = false;
  header.packet_sequence_number = GG_UINT64_C(0x123456789ABC);
  header.fec_group = 0;

  QuicConnectionCloseFrame close_frame;
  close_frame.error_code = static_cast<QuicErrorCode>(0x05060708);
  close_frame.error_details = "because I can";

  QuicAckFrame* ack_frame = &close_frame.ack_frame;
  ack_frame->received_info.entropy_hash = 0x43;
  ack_frame->received_info.largest_observed = GG_UINT64_C(0x0123456789ABF);
  ack_frame->received_info.missing_packets.insert(GG_UINT64_C(0x0123456789ABE));
  ack_frame->sent_info.entropy_hash = 0xE0;
  ack_frame->sent_info.least_unacked = GG_UINT64_C(0x0123456789AA0);

  QuicFrames frames;
  frames.push_back(QuicFrame(&close_frame));

  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags
    0x02,
    // first fec protected packet offset
    0xFF,

    // frame type (connection close frame)
    0x05,
    // error code
    0x08, 0x07, 0x06, 0x05,
    // error details length
    0x0d, 0x00,
    // error details
    'b',  'e',  'c',  'a',
    'u',  's',  'e',  ' ',
    'I',  ' ',  'c',  'a',
    'n',

    // Ack frame.
    // entropy hash of sent packets till least awaiting - 1.
    0xE0,
    // least packet sequence number awaiting an ack
    0xA0, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // entropy hash of all received packets.
    0x43,
    // largest observed packet sequence number
    0xBF, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // num missing packets
    0x01,
    // missing packet
    0xBE, 0x9A, 0x78, 0x56,
    0x34, 0x12,
  };

  scoped_ptr<QuicPacket> data(
      framer_.ConstructFrameDataPacket(header, frames).packet);
  ASSERT_TRUE(data != NULL);

  test::CompareCharArraysWithHexError("constructed packet",
                                      data->data(), data->length(),
                                      AsChars(packet), arraysize(packet));
}

TEST_F(QuicFramerTest, ConstructGoAwayPacket) {
  QuicPacketHeader header;
  header.public_header.guid = GG_UINT64_C(0xFEDCBA9876543210);
  header.public_header.reset_flag = false;
  header.public_header.version_flag = false;
  header.fec_flag = false;
  header.entropy_flag = true;
  header.fec_entropy_flag = false;
  header.packet_sequence_number = GG_UINT64_C(0x123456789ABC);
  header.fec_group = 0;

  QuicGoAwayFrame goaway_frame;
  goaway_frame.error_code = static_cast<QuicErrorCode>(0x05060708);
  goaway_frame.last_good_stream_id = 0x01020304;
  goaway_frame.reason_phrase = "because I can";

  QuicFrames frames;
  frames.push_back(QuicFrame(&goaway_frame));

  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags
    0x02,
    // first fec protected packet offset
    0xFF,

    // frame type (go away frame)
    0x06,
    // error code
    0x08, 0x07, 0x06, 0x05,
    // stream id
    0x04, 0x03, 0x02, 0x01,
    // error details length
    0x0d, 0x00,
    // error details
    'b',  'e',  'c',  'a',
    'u',  's',  'e',  ' ',
    'I',  ' ',  'c',  'a',
    'n',
  };

  scoped_ptr<QuicPacket> data(
      framer_.ConstructFrameDataPacket(header, frames).packet);
  ASSERT_TRUE(data != NULL);

  test::CompareCharArraysWithHexError("constructed packet",
                                      data->data(), data->length(),
                                      AsChars(packet), arraysize(packet));
}

TEST_F(QuicFramerTest, ConstructPublicResetPacket) {
  QuicPublicResetPacket reset_packet;
  reset_packet.public_header.guid = GG_UINT64_C(0xFEDCBA9876543210);
  reset_packet.public_header.reset_flag = true;
  reset_packet.public_header.version_flag = false;
  reset_packet.rejected_sequence_number = GG_UINT64_C(0x123456789ABC);
  reset_packet.nonce_proof = GG_UINT64_C(0xABCDEF0123456789);

  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x02,
    // nonce proof
    0x89, 0x67, 0x45, 0x23,
    0x01, 0xEF, 0xCD, 0xAB,
    // rejected sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
  };

  scoped_ptr<QuicEncryptedPacket> data(
      framer_.ConstructPublicResetPacket(reset_packet));
  ASSERT_TRUE(data != NULL);

  test::CompareCharArraysWithHexError("constructed packet",
                                      data->data(), data->length(),
                                      AsChars(packet), arraysize(packet));
}

TEST_F(QuicFramerTest, ConstructFecPacket) {
  QuicPacketHeader header;
  header.public_header.guid = GG_UINT64_C(0xFEDCBA9876543210);
  header.public_header.reset_flag = false;
  header.public_header.version_flag = false;
  header.fec_flag = true;
  header.entropy_flag = true;
  header.fec_entropy_flag = false;
  header.packet_sequence_number = (GG_UINT64_C(0x123456789ABC));
  header.fec_group = GG_UINT64_C(0x123456789ABB);;

  QuicFecData fec_data;
  fec_data.fec_group = 1;
  fec_data.redundancy = "abcdefghijklmnop";

  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags
    0x03,
    // first fec protected packet offset
    0x01,

    // redundancy
    'a',  'b',  'c',  'd',
    'e',  'f',  'g',  'h',
    'i',  'j',  'k',  'l',
    'm',  'n',  'o',  'p',
  };

  scoped_ptr<QuicPacket> data(
      framer_.ConstructFecPacket(header, fec_data).packet);
  ASSERT_TRUE(data != NULL);

  test::CompareCharArraysWithHexError("constructed packet",
                                      data->data(), data->length(),
                                      AsChars(packet), arraysize(packet));
}

TEST_F(QuicFramerTest, EncryptPacket) {
  QuicPacketSequenceNumber sequence_number = GG_UINT64_C(0x123456789ABC);
  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // private flags
    0x01,
    // first fec protected packet offset
    0x01,

    // redundancy
    'a',  'b',  'c',  'd',
    'e',  'f',  'g',  'h',
    'i',  'j',  'k',  'l',
    'm',  'n',  'o',  'p',
  };

  scoped_ptr<QuicPacket> raw(
      QuicPacket::NewDataPacket(AsChars(packet), arraysize(packet), false));
  scoped_ptr<QuicEncryptedPacket> encrypted(
      framer_.EncryptPacket(sequence_number, *raw));

  ASSERT_TRUE(encrypted.get() != NULL);
  EXPECT_TRUE(CheckEncryption(sequence_number,
                              StringPiece(AsChars(packet), arraysize(packet))));
}

// TODO(rch): re-enable after https://codereview.chromium.org/11820005/
// lands.  Currently this is causing valgrind problems, but it should be
// fixed in the followup CL.
TEST_F(QuicFramerTest, DISABLED_CalculateLargestReceived) {
  SequenceNumberSet missing;
  missing.insert(1);
  missing.insert(5);
  missing.insert(7);

  // These two we just walk to the next gap, and return the largest seen.
  EXPECT_EQ(4u, QuicFramer::CalculateLargestObserved(missing, missing.find(1)));
  EXPECT_EQ(6u, QuicFramer::CalculateLargestObserved(missing, missing.find(5)));

  missing.insert(2);
  // For 1, we can't go forward as 2 would be implicitly acked so we return the
  // largest missing packet.
  EXPECT_EQ(1u, QuicFramer::CalculateLargestObserved(missing, missing.find(1)));
  // For 2, we've seen 3 and 4, so can admit to a largest observed.
  EXPECT_EQ(4u, QuicFramer::CalculateLargestObserved(missing, missing.find(2)));
}

// TODO(rch) enable after landing the revised truncation CL.
TEST_F(QuicFramerTest, DISABLED_Truncation) {
  QuicPacketHeader header;
  header.public_header.guid = GG_UINT64_C(0xFEDCBA9876543210);
  header.public_header.reset_flag = false;
  header.public_header.version_flag = false;
  header.fec_flag = false;
  header.entropy_flag = false;
  header.fec_entropy_flag = false;
  header.packet_sequence_number = GG_UINT64_C(0x123456789ABC);
  header.fec_group = 0;

  QuicConnectionCloseFrame close_frame;
  QuicAckFrame* ack_frame = &close_frame.ack_frame;
  close_frame.error_code = static_cast<QuicErrorCode>(0x05060708);
  close_frame.error_details = "because I can";
  ack_frame->received_info.largest_observed = 201;
  ack_frame->sent_info.least_unacked = 0;
  for (uint64 i = 1; i < ack_frame->received_info.largest_observed; ++i) {
    ack_frame->received_info.missing_packets.insert(i);
  }

  // Create a packet with just the ack
  QuicFrame frame;
  frame.type = ACK_FRAME;
  frame.ack_frame = ack_frame;
  QuicFrames frames;
  frames.push_back(frame);

  scoped_ptr<QuicPacket> raw_ack_packet(
      framer_.ConstructFrameDataPacket(header, frames).packet);
  ASSERT_TRUE(raw_ack_packet != NULL);

  scoped_ptr<QuicEncryptedPacket> ack_packet(
      framer_.EncryptPacket(header.packet_sequence_number, *raw_ack_packet));

  // Create a packet with just connection close.
  frames.clear();
  frame.type = CONNECTION_CLOSE_FRAME;
  frame.connection_close_frame = &close_frame;
  frames.push_back(frame);

  scoped_ptr<QuicPacket> raw_close_packet(
      framer_.ConstructFrameDataPacket(header, frames).packet);
  ASSERT_TRUE(raw_close_packet != NULL);

  scoped_ptr<QuicEncryptedPacket> close_packet(
      framer_.EncryptPacket(header.packet_sequence_number, *raw_close_packet));

  // Now make sure we can turn our ack packet back into an ack frame
  ASSERT_TRUE(framer_.ProcessPacket(*ack_packet));

  // And do the same for the close frame.
  ASSERT_TRUE(framer_.ProcessPacket(*close_packet));
}

TEST_F(QuicFramerTest, CleanTruncation) {
  QuicPacketHeader header;
  header.public_header.guid = GG_UINT64_C(0xFEDCBA9876543210);
  header.public_header.reset_flag = false;
  header.public_header.version_flag = false;
  header.fec_flag = false;
  header.entropy_flag = true;
  header.fec_entropy_flag = false;
  header.packet_sequence_number = GG_UINT64_C(0x123456789ABC);
  header.fec_group = 0;

  QuicConnectionCloseFrame close_frame;
  QuicAckFrame* ack_frame = &close_frame.ack_frame;
  close_frame.error_code = static_cast<QuicErrorCode>(0x05060708);
  close_frame.error_details = "because I can";
  ack_frame->received_info.largest_observed = 201;
  ack_frame->sent_info.least_unacked = 0;
  for (uint64 i = 1; i < ack_frame->received_info.largest_observed; ++i) {
    ack_frame->received_info.missing_packets.insert(i);
  }

  // Create a packet with just the ack
  QuicFrame frame;
  frame.type = ACK_FRAME;
  frame.ack_frame = ack_frame;
  QuicFrames frames;
  frames.push_back(frame);

  scoped_ptr<QuicPacket> raw_ack_packet(
      framer_.ConstructFrameDataPacket(header, frames).packet);
  ASSERT_TRUE(raw_ack_packet != NULL);

  scoped_ptr<QuicEncryptedPacket> ack_packet(
      framer_.EncryptPacket(header.packet_sequence_number, *raw_ack_packet));

  // Create a packet with just connection close.
  frames.clear();
  frame.type = CONNECTION_CLOSE_FRAME;
  frame.connection_close_frame = &close_frame;
  frames.push_back(frame);

  scoped_ptr<QuicPacket> raw_close_packet(
      framer_.ConstructFrameDataPacket(header, frames).packet);
  ASSERT_TRUE(raw_close_packet != NULL);

  scoped_ptr<QuicEncryptedPacket> close_packet(
      framer_.EncryptPacket(header.packet_sequence_number, *raw_close_packet));

  // Now make sure we can turn our ack packet back into an ack frame
  ASSERT_TRUE(framer_.ProcessPacket(*ack_packet));

  // And do the same for the close frame.
  ASSERT_TRUE(framer_.ProcessPacket(*close_packet));

  // Test for clean truncation of the ack by comparing the length of the
  // original packets to the re-serialized packets.
  frames.clear();
  frame.type = ACK_FRAME;
  frame.ack_frame = visitor_.ack_frames_[0];
  frames.push_back(frame);

  size_t original_raw_length = raw_ack_packet->length();
  raw_ack_packet.reset(
      framer_.ConstructFrameDataPacket(header, frames).packet);
  ASSERT_TRUE(raw_ack_packet != NULL);
  EXPECT_EQ(original_raw_length, raw_ack_packet->length());

  frames.clear();
  frame.type = CONNECTION_CLOSE_FRAME;
  frame.connection_close_frame = &visitor_.connection_close_frame_;
  frames.push_back(frame);

  original_raw_length = raw_close_packet->length();
  raw_close_packet.reset(
      framer_.ConstructFrameDataPacket(header, frames).packet);
  ASSERT_TRUE(raw_ack_packet != NULL);
  EXPECT_EQ(original_raw_length, raw_close_packet->length());
}

TEST_F(QuicFramerTest, EntropyFlagTest) {
  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // Entropy
    0x02,
    // first fec protected packet offset
    0xFF,

    // frame type (stream frame)
    0x01,
    // stream id
    0x04, 0x03, 0x02, 0x01,
    // fin
    0x01,
    // offset
    0x54, 0x76, 0x10, 0x32,
    0xDC, 0xFE, 0x98, 0xBA,
    // data length
    0x0c, 0x00,
    // data
    'h',  'e',  'l',  'l',
    'o',  ' ',  'w',  'o',
    'r',  'l',  'd',  '!',
  };

  QuicEncryptedPacket encrypted(AsChars(packet), arraysize(packet), false);
  EXPECT_TRUE(framer_.ProcessPacket(encrypted));
  EXPECT_EQ(QUIC_NO_ERROR, framer_.error());
  ASSERT_TRUE(visitor_.header_.get());
  EXPECT_TRUE(visitor_.header_->entropy_flag);
  EXPECT_EQ(1 << 4, visitor_.header_->entropy_hash);
  EXPECT_FALSE(visitor_.header_->fec_flag);
};

TEST_F(QuicFramerTest, FecEntropyFlagTest) {
  unsigned char packet[] = {
    // guid
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    // public flags
    0x00,
    // packet sequence number
    0xBC, 0x9A, 0x78, 0x56,
    0x34, 0x12,
    // Flags: Entropy, FEC-Entropy, FEC
    0x07,
    // first fec protected packet offset
    0xFF,

    // frame type (stream frame)
    0x01,
    // stream id
    0x04, 0x03, 0x02, 0x01,
    // fin
    0x01,
    // offset
    0x54, 0x76, 0x10, 0x32,
    0xDC, 0xFE, 0x98, 0xBA,
    // data length
    0x0c, 0x00,
    // data
    'h',  'e',  'l',  'l',
    'o',  ' ',  'w',  'o',
    'r',  'l',  'd',  '!',
  };

  QuicEncryptedPacket encrypted(AsChars(packet), arraysize(packet), false);
  EXPECT_TRUE(framer_.ProcessPacket(encrypted));
  EXPECT_EQ(QUIC_NO_ERROR, framer_.error());
  ASSERT_TRUE(visitor_.header_.get());
  EXPECT_TRUE(visitor_.header_->fec_flag);
  EXPECT_TRUE(visitor_.header_->entropy_flag);
  EXPECT_TRUE(visitor_.header_->fec_entropy_flag);
  EXPECT_EQ(1 << 4, visitor_.header_->entropy_hash);
};

}  // namespace test
}  // namespace net
