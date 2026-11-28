#include "mcp/detail/server/elicitation_records.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace elicitation_records_test {

namespace {

/// A session of the test: the same session while `session` points at the same object.
struct Target {
    std::weak_ptr<int> session;
    int tag{0};

    [[nodiscard]] bool same_session(const Target& other) const {
        return !session.owner_before(other.session) && !other.session.owner_before(session);
    }
    [[nodiscard]] bool alive() const { return !session.expired(); }
};

using Records = mcp::detail::ElicitationRecords<Target>;
using std::chrono::hours;
using std::chrono::seconds;

class ElicitationRecordsTest : public ::testing::Test {
   protected:
    std::shared_ptr<int> first_ = std::make_shared<int>(1);
    std::shared_ptr<int> second_ = std::make_shared<int>(2);
    Records::Clock::time_point now_ = Records::Clock::now();

    [[nodiscard]] Target first(int tag = 0) const { return Target{first_, tag}; }
    [[nodiscard]] Target second() const { return Target{second_, 0}; }
};

}  // namespace

TEST_F(ElicitationRecordsTest, ARecordIsTakenOnce) {
    Records records(16, 4, hours(1));
    records.record("a", first(7), now_);
    const auto taken = records.take("a", now_);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(taken->tag, 7);
    EXPECT_FALSE(records.take("a", now_).has_value());
    EXPECT_FALSE(records.take("never", now_).has_value());
}

TEST_F(ElicitationRecordsTest, OnlyTheSessionThatSentAnIdTakesItAsItsOwn) {
    Records records(16, 4, hours(1));
    records.record("a", first(), now_);
    EXPECT_FALSE(records.take_own("a", second(), now_));
    // The other session's attempt leaves the record in place.
    EXPECT_TRUE(records.take_own("a", first(), now_));
    EXPECT_FALSE(records.take_own("a", first(), now_));
}

TEST_F(ElicitationRecordsTest, TheSameSessionReplacesItsRecordAndAnotherIsRefused) {
    Records records(16, 4, hours(1));
    records.record("a", first(1), now_);
    records.record("a", first(2), now_);
    EXPECT_EQ(records.size(), 1U);
    EXPECT_THROW(records.record("a", second(), now_), std::invalid_argument);
    const auto taken = records.take("a", now_);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(taken->tag, 2);
}

TEST_F(ElicitationRecordsTest, ARecordExpires) {
    Records records(16, 4, seconds(10));
    records.record("a", first(), now_);
    records.record("b", first(), now_ + seconds(5));
    EXPECT_FALSE(records.take("a", now_ + seconds(10)).has_value());
    EXPECT_TRUE(records.take("b", now_ + seconds(10)).has_value());
}

TEST_F(ElicitationRecordsTest, AnExpiredRecordOfAnotherSessionFreesItsId) {
    Records records(16, 4, seconds(10));
    records.record("a", first(), now_);
    EXPECT_NO_THROW(records.record("a", second(), now_ + seconds(10)));
}

TEST_F(ElicitationRecordsTest, ASessionAtItsCapLosesItsOldestRecord) {
    Records records(16, 2, hours(1));
    records.record("other", second(), now_);
    records.record("a", first(), now_);
    records.record("b", first(), now_);
    records.record("c", first(), now_);
    EXPECT_FALSE(records.take("a", now_).has_value());
    EXPECT_TRUE(records.take("b", now_).has_value());
    EXPECT_TRUE(records.take("c", now_).has_value());
    // The other session's record does not count against this session's cap.
    EXPECT_TRUE(records.take("other", now_).has_value());
}

TEST_F(ElicitationRecordsTest, AtTheServersCapTheOldestRecordGoes) {
    Records records(3, 3, hours(1));
    records.record("a", first(), now_);
    records.record("b", second(), now_);
    records.record("c", first(), now_);
    records.record("d", second(), now_);
    EXPECT_EQ(records.size(), 3U);
    EXPECT_FALSE(records.take("a", now_).has_value());
    EXPECT_TRUE(records.take("b", now_).has_value());
}

TEST_F(ElicitationRecordsTest, TheRecordsOfAnEndedSessionAreGone) {
    Records records(16, 4, hours(1));
    records.record("a", first(), now_);
    records.record("b", second(), now_);
    first_.reset();
    EXPECT_FALSE(records.take("a", now_).has_value());
    EXPECT_EQ(records.size(), 1U);
    records.forget(second());
    EXPECT_EQ(records.size(), 0U);
}

}  // namespace elicitation_records_test
