//
//  ReplicatorLoopbackTest.hh
//  LiteCore
//
//  Created by Jens Alfke on 7/12/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "fleece/Fleece.hh"
#include "c4.hh"
#include "c4Document+Fleece.h"
#include "Replicator.hh"
#include "LoopbackProvider.hh"
#include "ReplicatorTuning.hh"
#include "StringUtil.hh"
#include <algorithm>
#include <chrono>
#include <thread>

#include "c4Test.hh"


using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::repl;
using namespace litecore::websocket;


class ReplicatorLoopbackTest : public C4Test, Replicator::Delegate {
public:
    static constexpr duration kLatency              = chrono::milliseconds(50);


    ReplicatorLoopbackTest()
    :C4Test(0)
    ,db2(createDatabase("2"))
    {
        // Change tuning param so that tests will actually create deltas, despite using small
        // document bodies:
        litecore::repl::tuning::kMinBodySizeForDelta = 0;
    }

    ~ReplicatorLoopbackTest() {
        if (_parallelThread)
            _parallelThread->join();
        _replClient = _replServer = nullptr;
        C4Error error;
        REQUIRE(c4db_delete(db2, &error));
        c4db_free(db2);
    }

    // opts1 is the options for _db; opts2 is the options for _db2
    void runReplicators(Replicator::Options opts1, Replicator::Options opts2) {
        _gotResponse = false;
        _statusChangedCalls = 0;
        _statusReceived = {};
        _replicatorClientFinished = _replicatorServerFinished = false;

        C4Database *dbClient = db, *dbServer = db2;
        if (opts2.push > kC4Passive || opts2.pull > kC4Passive) {
            // always make opts1 the active (client) side
            swap(dbServer, dbClient);
            swap(opts1, opts2);
        }

        // Create client (active) and server (passive) replicators:
        _replClient = new Replicator(dbClient,
                                     new LoopbackWebSocket(alloc_slice("ws://srv/"_sl), Role::Client, kLatency),
                                     *this, opts1);
        _replServer = new Replicator(dbServer,
                                     new LoopbackWebSocket(alloc_slice("ws://cli/"_sl), Role::Server, kLatency),
                                     *this, opts2);

        // Response headers:
        Encoder enc;
        enc.beginDict();
        enc.writeKey("Set-Cookie"_sl);
        enc.writeString("flavor=chocolate-chip");
        enc.endDict();
        AllocedDict headers(enc.finish());

        // Bind the replicators' WebSockets and start them:
        LoopbackWebSocket::bind(_replClient->webSocket(), _replServer->webSocket(), headers);
        Stopwatch st;
        _replClient->start();
        _replServer->start();

        {
            Log("Waiting for replication to complete...");
            unique_lock<mutex> lock(_mutex);
            while (!_replicatorClientFinished || !_replicatorServerFinished)
                _cond.wait(lock);
        }
        
        Log(">>> Replication complete (%.3f sec) <<<", st.elapsed());
        _checkpointID = _replClient->checkpointID();
        _replClient = _replServer = nullptr;

        CHECK(_gotResponse);
        CHECK(_statusChangedCalls > 0);
        CHECK(_statusReceived.level == kC4Stopped);
        CHECK(_statusReceived.progress.unitsCompleted == _statusReceived.progress.unitsTotal);
        if(_expectedUnitsComplete >= 0)
            CHECK(_expectedUnitsComplete == _statusReceived.progress.unitsCompleted);
        if (_expectedDocumentCount >= 0)
            CHECK(_statusReceived.progress.documentCount == uint64_t(_expectedDocumentCount));
        CHECK(_statusReceived.error.code == _expectedError.code);
        if (_expectedError.code)
            CHECK(_statusReceived.error.domain == _expectedError.domain);
        CHECK(asVector(_docPullErrors) == asVector(_expectedDocPullErrors));
        CHECK(asVector(_docPushErrors) == asVector(_expectedDocPushErrors));
        CHECK(asVector(_docsFinished) == asVector(_expectedDocsFinished));
    }

    void runPushReplication(C4ReplicatorMode mode =kC4OneShot) {
        runReplicators(Replicator::Options::pushing(mode), Replicator::Options::passive());
    }

    void runPullReplication(C4ReplicatorMode mode =kC4OneShot) {
        runReplicators(Replicator::Options::passive(), Replicator::Options::pulling(mode));
    }

    void runPushPullReplication(C4ReplicatorMode mode =kC4OneShot) {
        runReplicators(Replicator::Options(mode, mode), Replicator::Options::passive());
    }

    virtual void replicatorGotHTTPResponse(Replicator *repl, int status,
                                           const AllocedDict &headers) override {
        // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
        if (repl == _replClient) {
            Assert(!_gotResponse);
            _gotResponse = true;
            Assert(status == 200);
            Assert(headers["Set-Cookie"].asString() == "flavor=chocolate-chip"_sl);
        }
    }

    virtual void replicatorStatusChanged(Replicator* repl,
                                         const Replicator::Status &status) override
    {
        // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
        if (repl == _replClient) {
            Assert(_gotResponse);
            ++_statusChangedCalls;
            Log(">> Replicator is %-s, progress %lu/%lu, %lu docs",
                kC4ReplicatorActivityLevelNames[status.level],
                (unsigned long)status.progress.unitsCompleted,
                (unsigned long)status.progress.unitsTotal,
                (unsigned long)status.progress.documentCount);
            Assert(status.progress.unitsCompleted <= status.progress.unitsTotal);
            Assert(status.progress.documentCount < 1000000);
            if (status.progress.unitsTotal > 0) {
                Assert(status.progress.unitsCompleted >= _statusReceived.progress.unitsCompleted);
                Assert(status.progress.unitsTotal     >= _statusReceived.progress.unitsTotal);
                Assert(status.progress.documentCount  >= _statusReceived.progress.documentCount);
            }
            _statusReceived = status;

            if (_stopOnIdle && status.level == kC4Idle && (_expectedDocumentCount <= 0 || status.progress.documentCount == _expectedDocumentCount)) {
                Log(">>    Stopping idle replicator...");
                repl->stop();
            }
        }

        if (status.level == kC4Stopped) {
            unique_lock<mutex> lock(_mutex);
            if (repl == _replClient)
                _replicatorClientFinished = true;
            else
                _replicatorServerFinished = true;
            if (_replicatorClientFinished && _replicatorServerFinished)
                _cond.notify_all();
        }
    }

    virtual void replicatorDocumentEnded(Replicator *repl,
                                         Dir dir,
                                         slice docID,
                                         C4Error error,
                                         bool transient) override
    {
        if (error.code) {
        // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
        char message[256];
        c4error_getDescriptionC(error, message, sizeof(message));
        Log(">> Replicator %serror %s '%.*s': %s",
            (transient ? "transient " : ""),
                (dir == Dir::kPushing ? "pushing" : "pulling"),
            SPLAT(docID), message);
            if (dir == Dir::kPushing)
            _docPushErrors.emplace(docID);
        else
            _docPullErrors.emplace(docID);
        } else {
            Log(">> Replicator %s '%.*s'",
                (dir == Dir::kPushing ? "pushed" : "pulled"), SPLAT(docID));
            _docsFinished.emplace(docID);
        }
    }

    virtual void replicatorBlobProgress(Replicator *repl,
                                        const Replicator::BlobProgress &p) override
    {
        if (p.dir == Dir::kPushing) {
            ++_blobPushProgressCallbacks;
            _lastBlobPushProgress = p;
        } else {
            ++_blobPullProgressCallbacks;
            _lastBlobPullProgress = p;
        }
        alloc_slice keyString(c4blob_keyToString(p.key));
        Log(">> Replicator %s blob '%.*s'%.*s [%.*s] (%llu / %llu)",
            (p.dir == Dir::kPushing ? "pushing" : "pulling"), SPLAT(p.docID),
            SPLAT(p.docProperty), SPLAT(keyString),
            p.bytesCompleted, p.bytesTotal);
    }

    virtual void replicatorConnectionClosed(Replicator* repl, const CloseStatus &status) override {
        // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
        if (repl == _replClient) {
            Log(">> Replicator closed with code=%d/%d, message=%.*s",
                status.reason, status.code, SPLAT(status.message));
        }
    }


    void runInParallel(function<void(C4Database*)> callback) {
        C4Error error;
        C4Database *parallelDB = c4db_openAgain(db, &error);
        REQUIRE(parallelDB != nullptr);

        _parallelThread.reset(new thread([=]() mutable {
            callback(parallelDB);
            c4db_free(parallelDB);
        }));
    }

    void addDocsInParallel(duration interval, int total) {
        runInParallel([=](C4Database *bgdb) {
            // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
            int docNo = 1;
            for (int i = 1; docNo <= total; i++) {
                this_thread::sleep_for(interval);
                Log("-------- Creating %d docs --------", 2*i);
                c4::Transaction t(bgdb);
                C4Error err;
                Assert(t.begin(&err));
                for (int j = 0; j < 2*i; j++) {
                    char docID[20];
                    sprintf(docID, "newdoc%d", docNo++);
                    createRev(bgdb, c4str(docID), "1-11"_sl, kFleeceBody);
                }
                Assert(t.commit(&err));
            }
            Log("-------- Done creating docs --------");
            _expectedDocumentCount = docNo - 1;
            _stopOnIdle = true;
        });
    }

    void addRevsInParallel(duration interval, alloc_slice docID, int firstRev, int totalRevs) {
        runInParallel([=](C4Database *bgdb) {
            for (int i = 0; i < totalRevs; i++) {
                // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
                int revNo = firstRev + i;
                this_thread::sleep_for(interval);
                Log("-------- Creating rev %.*s # %d --------", SPLAT(docID), revNo);
                c4::Transaction t(bgdb);
                C4Error err;
                Assert(t.begin(&err));
                char revID[20];
                sprintf(revID, "%d-ffff", revNo);
                createRev(bgdb, docID, c4str(revID), kFleeceBody);
                Assert(t.commit(&err));
            }
            Log("-------- Done creating revs --------");
            _stopOnIdle = true;
        });
    }

#define fastREQUIRE(EXPR)  if (EXPR) ; else REQUIRE(EXPR)       // REQUIRE() is kind of expensive

    void compareDocs(C4Document *doc1, C4Document *doc2) {
        const auto kPublicDocumentFlags = (kDocDeleted | kDocConflicted | kDocHasAttachments);

        fastREQUIRE(doc1->docID == doc2->docID);
        fastREQUIRE(doc1->revID == doc2->revID);
        fastREQUIRE((doc1->flags & kPublicDocumentFlags) == (doc2->flags & kPublicDocumentFlags));

        // Compare canonical JSON forms of both docs:
        Doc rev1 = c4::getFleeceDoc(doc1), rev2 = c4::getFleeceDoc(doc2);
        if (!rev1.root().isEqual(rev2.root())) {        // fast check to avoid expensive toJSON
            alloc_slice json1 = rev1.root().toJSON(true, true);
            alloc_slice json2 = rev2.root().toJSON(true, true);
            CHECK(json1 == json2);
        }
    }

    void compareDatabases(bool db2MayHaveMoreDocs =false, bool compareDeletedDocs =true) {
        C4Log(">> Comparing databases...");
        C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
        if (compareDeletedDocs)
            options.flags |= kC4IncludeDeleted;
        C4Error error;
        c4::ref<C4DocEnumerator> e1 = c4db_enumerateAllDocs(db, &options, &error);
        REQUIRE(e1);
        c4::ref<C4DocEnumerator> e2 = c4db_enumerateAllDocs(db2, &options, &error);
        REQUIRE(e2);

        unsigned i = 0;
        while (c4enum_next(e1, &error)) {
            c4::ref<C4Document> doc1 = c4enum_getDocument(e1, &error);
            fastREQUIRE(doc1);
            INFO("db document #" << i << ": '" << slice(doc1->docID).asString() << "'");
            bool ok = c4enum_next(e2, &error);
            fastREQUIRE(ok);
            c4::ref<C4Document> doc2 = c4enum_getDocument(e2, &error);
            fastREQUIRE(doc2);
            compareDocs(doc1, doc2);
            ++i;
        }
        REQUIRE(error.code == 0);
        if (!db2MayHaveMoreDocs) {
            REQUIRE(!c4enum_next(e2, &error));
            REQUIRE(error.code == 0);
        }
    }

    void validateCheckpoint(C4Database *database, bool local,
                            const char *body, const char *meta = "1-") {
        C4Error err;
		C4Slice storeName;
		if(local) {
			storeName = C4STR("checkpoints");
		} else {
			storeName = C4STR("peerCheckpoints");
		}

        c4::ref<C4RawDocument> doc( c4raw_get(database,
                                              storeName,
                                              _checkpointID,
                                              &err) );
        INFO("Checking " << (local ? "local" : "remote") << " checkpoint '" << string(_checkpointID) << "'; err = " << err.domain << "," << err.code);
        REQUIRE(doc);
        CHECK(doc->body == c4str(body));
        if (!local)
            CHECK(c4rev_getGeneration(doc->meta) >= c4rev_getGeneration(c4str(meta)));
    }

    void validateCheckpoints(C4Database *localDB, C4Database *remoteDB,
                             const char *body, const char *meta = "1-cc") {
        validateCheckpoint(localDB,  true,  body, meta);
        validateCheckpoint(remoteDB, false, body, meta);
    }

    void clearCheckpoint(C4Database *database, bool local) {
        C4Error err;
		C4Slice storeName;
		if(local) {
			storeName = C4STR("checkpoints");
		} else {
			storeName = C4STR("peerCheckpoints");
		}

        REQUIRE( c4raw_put(database,
                           storeName,
                           _checkpointID,
                           kC4SliceNull, kC4SliceNull, &err) );
    }

    template <class SET>
    static vector<string> asVector(const SET &strings) {
        vector<string> out;
        for (const string &s : strings)
            out.push_back(s);
        return out;
    }

    C4Database* db2 {nullptr};
    Retained<Replicator> _replClient, _replServer;
    alloc_slice _checkpointID;
    unique_ptr<thread> _parallelThread;
    atomic<bool> _stopOnIdle {0};
    mutex _mutex;
    condition_variable _cond;
    bool _replicatorClientFinished {false}, _replicatorServerFinished {false};
    bool _gotResponse {false};
    Replicator::Status _statusReceived { };
    unsigned _statusChangedCalls {0};
    int64_t _expectedDocumentCount {0};
    int64_t _expectedUnitsComplete {-1};
    C4Error _expectedError {};
    set<string> _docPushErrors, _docPullErrors;
    set<string> _expectedDocPushErrors, _expectedDocPullErrors;
    multiset<string> _docsFinished, _expectedDocsFinished;
    unsigned _blobPushProgressCallbacks {0}, _blobPullProgressCallbacks {0};
    Replicator::BlobProgress _lastBlobPushProgress {}, _lastBlobPullProgress {};
};

