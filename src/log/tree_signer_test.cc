/* -*- indent-tabs-mode: nil -*- */

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <stdint.h>
#include <string>

#include "ct.pb.h"
#include "file_db.h"
#include "log_signer.h"
#include "log_verifier.h"
#include "merkle_verifier.h"
#include "sqlite_db.h"
#include "test_db.h"
#include "test_signer.h"
#include "tree_signer.h"
#include "util.h"

namespace {

using ct::LoggedCertificate;
using ct::SignedTreeHead;
using std::string;

template <class T> class TreeSignerTest : public ::testing::Test {
 protected:
  TreeSignerTest()
      : test_db_(),
        test_signer_(),
        verifier_(NULL),
        tree_signer_(NULL) {}

  void SetUp() {
    verifier_ = new LogVerifier(TestSigner::DefaultVerifier(),
                                new MerkleVerifier(new Sha256Hasher()));
    tree_signer_ = new TreeSigner(db(), TestSigner::DefaultSigner());
  }

  TreeSigner *GetSimilar() const {
    return new TreeSigner(db(), TestSigner::DefaultSigner());
  }

  ~TreeSignerTest() {
    delete verifier_;
    delete tree_signer_;
  }

  T *db() const { return test_db_.db(); }
  TestDB<T> test_db_;
  TestSigner test_signer_;
  LogVerifier *verifier_;
  TreeSigner *tree_signer_;
};

typedef testing::Types<FileDB, SQLiteDB> Databases;

TYPED_TEST_CASE(TreeSignerTest, Databases);

// TODO(ekasper): KAT tests.
TYPED_TEST(TreeSignerTest, Sign) {
  LoggedCertificate logged_cert;
  this->test_signer_.CreateUnique(&logged_cert);
  EXPECT_EQ(Database::OK,
            this->db()->CreatePendingCertificateEntry(logged_cert));

  EXPECT_EQ(TreeSigner::OK, this->tree_signer_->UpdateTree());

  SignedTreeHead sth;
  EXPECT_EQ(Database::LOOKUP_OK, this->db()->LatestTreeHead(&sth));
  EXPECT_EQ(1U, sth.tree_size());
  EXPECT_EQ(sth.timestamp(), this->tree_signer_->LastUpdateTime());
}

TYPED_TEST(TreeSignerTest, Timestamp) {
  LoggedCertificate logged_cert;
  this->test_signer_.CreateUnique(&logged_cert);
  EXPECT_EQ(Database::OK,
            this->db()->CreatePendingCertificateEntry(logged_cert));

  EXPECT_EQ(TreeSigner::OK, this->tree_signer_->UpdateTree());
  uint64_t last_update = this->tree_signer_->LastUpdateTime();
  EXPECT_GE(last_update, logged_cert.sct().timestamp());

  // Now create a second entry with a timestamp some time in the future
  // and verify that the signer's timestamp is greater than that.
  uint64_t future = last_update + 10000;
  LoggedCertificate logged_cert2;
  this->test_signer_.CreateUnique(&logged_cert2);
  logged_cert2.mutable_sct()->set_timestamp(future);
  EXPECT_EQ(Database::OK,
            this->db()->CreatePendingCertificateEntry(logged_cert2));

  EXPECT_EQ(TreeSigner::OK, this->tree_signer_->UpdateTree());
  EXPECT_GE(this->tree_signer_->LastUpdateTime(), future);
}

TYPED_TEST(TreeSignerTest, Verify) {
  LoggedCertificate logged_cert;
  this->test_signer_.CreateUnique(&logged_cert);
  EXPECT_EQ(Database::OK,
            this->db()->CreatePendingCertificateEntry(logged_cert));

  EXPECT_EQ(TreeSigner::OK, this->tree_signer_->UpdateTree());

  SignedTreeHead sth;
  EXPECT_EQ(Database::LOOKUP_OK, this->db()->LatestTreeHead(&sth));
  EXPECT_EQ(LogVerifier::VERIFY_OK, this->verifier_->VerifySignedTreeHead(sth));
}

TYPED_TEST(TreeSignerTest, ResumeClean) {
  LoggedCertificate logged_cert;
  this->test_signer_.CreateUnique(&logged_cert);
  EXPECT_EQ(Database::OK,
            this->db()->CreatePendingCertificateEntry(logged_cert));

  EXPECT_EQ(TreeSigner::OK, this->tree_signer_->UpdateTree());
  SignedTreeHead sth;

  EXPECT_EQ(Database::LOOKUP_OK, this->db()->LatestTreeHead(&sth));

  TreeSigner *signer2 = this->GetSimilar();
  EXPECT_EQ(signer2->LastUpdateTime(), sth.timestamp());

  // Update
  EXPECT_EQ(TreeSigner::OK, signer2->UpdateTree());
  SignedTreeHead sth2;

  EXPECT_EQ(Database::LOOKUP_OK, this->db()->LatestTreeHead(&sth2));
  EXPECT_LT(sth.timestamp(), sth2.timestamp());
  EXPECT_EQ(sth.root_hash(), sth2.root_hash());
  EXPECT_EQ(sth.tree_size(), sth2.tree_size());

  delete signer2;
}

// Test resuming when the tree head signature is lagging behind the
// sequence number commits.
TYPED_TEST(TreeSignerTest, ResumePartialSign) {
  EXPECT_EQ(TreeSigner::OK, this->tree_signer_->UpdateTree());
  SignedTreeHead sth;
  EXPECT_EQ(Database::LOOKUP_OK, this->db()->LatestTreeHead(&sth));

  LoggedCertificate logged_cert;
  this->test_signer_.CreateUnique(&logged_cert);
  EXPECT_EQ(Database::OK,
            this->db()->CreatePendingCertificateEntry(logged_cert));

  // Simulate the case where we assign a sequence number but fail
  // before signing.
  EXPECT_EQ(Database::OK,
            this->db()->AssignCertificateSequenceNumber(
                logged_cert.certificate_sha256_hash(), 0));

  TreeSigner *signer2 = this->GetSimilar();
  EXPECT_EQ(TreeSigner::OK, signer2->UpdateTree());
  SignedTreeHead sth2;
  EXPECT_EQ(Database::LOOKUP_OK, this->db()->LatestTreeHead(&sth2));
  // The signer should have picked up the sequence number commit.
  EXPECT_EQ(1U, sth2.tree_size());
  EXPECT_LT(sth.timestamp(), sth2.timestamp());
  EXPECT_NE(sth.root_hash(), sth2.root_hash());

  delete signer2;
}

TYPED_TEST(TreeSignerTest, SignEmpty) {
  EXPECT_EQ(TreeSigner::OK, this->tree_signer_->UpdateTree());
  SignedTreeHead sth;

  EXPECT_EQ(Database::LOOKUP_OK, this->db()->LatestTreeHead(&sth));
  EXPECT_GT(sth.timestamp(), 0U);
  EXPECT_EQ(sth.tree_size(), 0U);
}

TYPED_TEST(TreeSignerTest, FailInconsistentTreeHead) {
  EXPECT_EQ(TreeSigner::OK, this->tree_signer_->UpdateTree());
  // A second signer interferes.
  TreeSigner *signer2 = this->GetSimilar();
  EXPECT_EQ(TreeSigner::OK, signer2->UpdateTree());
  // The first signer should detect this and refuse to update.
  EXPECT_EQ(TreeSigner::DB_ERROR, this->tree_signer_->UpdateTree());

  delete signer2;
}

TYPED_TEST(TreeSignerTest, FailInconsistentSequenceNumbers) {
  EXPECT_EQ(TreeSigner::OK, this->tree_signer_->UpdateTree());

  LoggedCertificate logged_cert;
  this->test_signer_.CreateUnique(&logged_cert);
  EXPECT_EQ(Database::OK,
            this->db()->CreatePendingCertificateEntry(logged_cert));

  // Assign a sequence number the signer does not know about.
  EXPECT_EQ(Database::OK,
            this->db()->AssignCertificateSequenceNumber(
                logged_cert.certificate_sha256_hash(), 0));

  // Create another pending entry.
  LoggedCertificate logged_cert2;
  this->test_signer_.CreateUnique(&logged_cert2);
  EXPECT_EQ(Database::OK,
            this->db()->CreatePendingCertificateEntry(logged_cert2));

  // Update should fail because we cannot commit a sequence number.
  EXPECT_EQ(TreeSigner::DB_ERROR, this->tree_signer_->UpdateTree());
}

}  // namespace

int main(int argc, char **argv) {
  // Change the defaults. Can be overridden on command line.
  // Log to stderr instead of log files.
  FLAGS_logtostderr = true;
  // Only log fatal messages by default.
  FLAGS_minloglevel = 3;
  google::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}