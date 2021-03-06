// Copyright (C) 2012  Internet Systems Consortium, Inc. ("ISC")
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
// REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
// AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
// INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
// LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
// OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#include <datasrc/memory/zone_writer_local.h>
#include <datasrc/memory/zone_table_segment_local.h>
#include <datasrc/memory/zone_data.h>

#include <dns/rrclass.h>
#include <dns/name.h>

#include <gtest/gtest.h>

#include <boost/scoped_ptr.hpp>
#include <boost/bind.hpp>

using boost::scoped_ptr;
using boost::bind;
using isc::dns::RRClass;
using isc::dns::Name;
using namespace isc::datasrc::memory;

namespace {

class TestException {};

class ZoneWriterLocalTest : public ::testing::Test {
public:
    ZoneWriterLocalTest() :
        segment_(ZoneTableSegment::create(RRClass::IN(), "local")),
        writer_(new
            ZoneWriterLocal(dynamic_cast<ZoneTableSegmentLocal*>(segment_.
                                                                 get()),
                            bind(&ZoneWriterLocalTest::loadAction, this, _1),
                            Name("example.org"), RRClass::IN())),
        load_called_(false),
        load_throw_(false),
        load_null_(false),
        load_data_(false)
    {}
    void TearDown() {
        // Release the writer
        writer_.reset();
    }
protected:
    scoped_ptr<ZoneTableSegment> segment_;
    scoped_ptr<ZoneWriterLocal> writer_;
    bool load_called_;
    bool load_throw_;
    bool load_null_;
    bool load_data_;
private:
    ZoneData* loadAction(isc::util::MemorySegment& segment) {
        // Make sure it is the correct segment passed. We know the
        // exact instance, can compare pointers to them.
        EXPECT_EQ(&segment_->getMemorySegment(), &segment);
        // We got called
        load_called_ = true;
        if (load_throw_) {
            throw TestException();
        }

        if (load_null_) {
            // Be nasty to the caller and return NULL, which is forbidden
            return (NULL);
        }
        ZoneData* data = ZoneData::create(segment, Name("example.org"));
        if (load_data_) {
            // Put something inside. The node itself should be enough for
            // the tests.
            ZoneNode* node(NULL);
            data->insertName(segment, Name("subdomain.example.org"), &node);
            EXPECT_NE(static_cast<ZoneNode*>(NULL), node);
        }
        return (data);
    }
};

// We call it the way we are supposed to, check every callback is called in the
// right moment.
TEST_F(ZoneWriterLocalTest, correctCall) {
    // Nothing called before we call it
    EXPECT_FALSE(load_called_);

    // Just the load gets called now
    EXPECT_NO_THROW(writer_->load());
    EXPECT_TRUE(load_called_);
    load_called_ = false;

    EXPECT_NO_THROW(writer_->install());
    EXPECT_FALSE(load_called_);

    // We don't check explicitly how this works, but call it to free memory. If
    // everything is freed should be checked inside the TearDown.
    EXPECT_NO_THROW(writer_->cleanup());
}

TEST_F(ZoneWriterLocalTest, loadTwice) {
    // Load it the first time
    EXPECT_NO_THROW(writer_->load());
    EXPECT_TRUE(load_called_);
    load_called_ = false;

    // The second time, it should not be possible
    EXPECT_THROW(writer_->load(), isc::InvalidOperation);
    EXPECT_FALSE(load_called_);

    // The object should not be damaged, try installing and clearing now
    EXPECT_NO_THROW(writer_->install());
    EXPECT_FALSE(load_called_);

    // We don't check explicitly how this works, but call it to free memory. If
    // everything is freed should be checked inside the TearDown.
    EXPECT_NO_THROW(writer_->cleanup());
}

// Try loading after call to install and call to cleanup. Both is
// forbidden.
TEST_F(ZoneWriterLocalTest, loadLater) {
    // Load first, so we can install
    EXPECT_NO_THROW(writer_->load());
    EXPECT_NO_THROW(writer_->install());
    // Reset so we see nothing is called now
    load_called_ = false;

    EXPECT_THROW(writer_->load(), isc::InvalidOperation);
    EXPECT_FALSE(load_called_);

    // Cleanup and try loading again. Still shouldn't work.
    EXPECT_NO_THROW(writer_->cleanup());

    EXPECT_THROW(writer_->load(), isc::InvalidOperation);
    EXPECT_FALSE(load_called_);
}

// Try calling install at various bad times
TEST_F(ZoneWriterLocalTest, invalidInstall) {
    // Nothing loaded yet
    EXPECT_THROW(writer_->install(), isc::InvalidOperation);
    EXPECT_FALSE(load_called_);

    EXPECT_NO_THROW(writer_->load());
    load_called_ = false;
    // This install is OK
    EXPECT_NO_THROW(writer_->install());
    // But we can't call it second time now
    EXPECT_THROW(writer_->install(), isc::InvalidOperation);
    EXPECT_FALSE(load_called_);
}

// We check we can clean without installing first and nothing bad
// happens. We also misuse the testcase to check we can't install
// after cleanup.
TEST_F(ZoneWriterLocalTest, cleanWithoutInstall) {
    EXPECT_NO_THROW(writer_->load());
    EXPECT_NO_THROW(writer_->cleanup());

    EXPECT_TRUE(load_called_);

    // We cleaned up, no way to install now
    EXPECT_THROW(writer_->install(), isc::InvalidOperation);
}

// Test the case when load callback throws
TEST_F(ZoneWriterLocalTest, loadThrows) {
    load_throw_ = true;
    EXPECT_THROW(writer_->load(), TestException);

    // We can't install now
    EXPECT_THROW(writer_->install(), isc::InvalidOperation);
    EXPECT_TRUE(load_called_);

    // But we can cleanup
    EXPECT_NO_THROW(writer_->cleanup());
}

// Check the strong exception guarantee - if it throws, nothing happened
// to the content.
TEST_F(ZoneWriterLocalTest, retry) {
    // First attempt fails due to some exception.
    load_throw_ = true;
    EXPECT_THROW(writer_->load(), TestException);
    // This one shall succeed.
    load_called_ = load_throw_ = false;
    // We want some data inside.
    load_data_ = true;
    EXPECT_NO_THROW(writer_->load());
    // And this one will fail again. But the old data will survive.
    load_data_ = false;
    EXPECT_THROW(writer_->load(), isc::InvalidOperation);

    // The rest still works correctly
    EXPECT_NO_THROW(writer_->install());
    ZoneTable* const table(segment_->getHeader().getTable());
    const ZoneTable::FindResult found(table->findZone(Name("example.org")));
    ASSERT_EQ(isc::datasrc::result::SUCCESS, found.code);
    // For some reason it doesn't seem to work by the ZoneNode typedef, using
    // the full definition instead. At least on some compilers.
    const isc::datasrc::memory::DomainTreeNode<RdataSet>* node;
    EXPECT_EQ(isc::datasrc::memory::DomainTree<RdataSet>::EXACTMATCH,
              found.zone_data->getZoneTree().
              find(Name("subdomain.example.org"), &node));
    EXPECT_NO_THROW(writer_->cleanup());
}

// Check the writer defends itsefl when load action returns NULL
TEST_F(ZoneWriterLocalTest, loadNull) {
    load_null_ = true;
    EXPECT_THROW(writer_->load(), isc::InvalidOperation);

    // We can't install that
    EXPECT_THROW(writer_->install(), isc::InvalidOperation);

    // It should be possible to clean up safely
    EXPECT_NO_THROW(writer_->cleanup());
}

// Check the object cleans up in case we forget it.
TEST_F(ZoneWriterLocalTest, autoCleanUp) {
    // Load data and forget about it. It should get released
    // when the writer itself is destroyed.
    EXPECT_NO_THROW(writer_->load());
}

}
