#include "test/lsp/ProtocolTest.h"
#include "absl/strings/str_split.h" // For StripAsciiWhitespace
#include "test/helpers/MockFileSystem.h"
#include "test/helpers/lsp.h"
#include "test/helpers/position_assertions.h"
#include <filesystem>

using namespace std;

string exec(string cmd);

namespace sorbet::test::lsp {
namespace {
bool isSorbetFence(const LSPMessage &msg) {
    return msg.isNotification() && msg.method() == LSPMethod::SorbetFence;
}

bool isTypecheckRun(const LSPMessage &msg) {
    return msg.isNotification() && msg.method() == LSPMethod::SorbetTypecheckRunInfo;
}
} // namespace

CounterStateDatabase::CounterStateDatabase(CounterState counters) : counters(move(counters)) {
    CHECK_FALSE(this->counters.hasNullCounters());
    // Combines counters with the same name but different char* pointers.
    this->counters.counters->canonicalize();
}

// Get counter value or 0.
CounterImpl::CounterType CounterStateDatabase::getCounter(ConstExprStr counter) const {
    auto internedCounter = counters.counters->internKey(counter.str);
    const auto &it = counters.counters->counters.find(internedCounter);
    if (it != counters.counters->counters.end()) {
        return it->second;
    }
    return 0;
}

CounterImpl::CounterType CounterStateDatabase::getCategoryCounter(ConstExprStr counter, ConstExprStr category) const {
    auto internedCounter = counters.counters->internKey(counter.str);
    const auto &it = counters.counters->countersByCategory.find(internedCounter);
    if (it == counters.counters->countersByCategory.end()) {
        return 0;
    }
    auto internedCategory = counters.counters->internKey(category.str);
    const auto &catIt = it->second.find(internedCategory);
    if (catIt == it->second.end()) {
        return 0;
    }
    return catIt->second;
}

CounterImpl::CounterType CounterStateDatabase::getCategoryCounterSum(ConstExprStr counter) const {
    auto internedCounter = counters.counters->internKey(counter.str);
    const auto &it = counters.counters->countersByCategory.find(internedCounter);
    CounterImpl::CounterType total = 0;
    if (it != counters.counters->countersByCategory.end()) {
        for (const auto &catIt : it->second) {
            total += catIt.second;
        }
    }
    return total;
}

CounterImpl::CounterType CounterStateDatabase::getHistogramCount(ConstExprStr histogram) const {
    auto internedHistogram = counters.counters->internKey(histogram.str);
    CounterImpl::CounterType rv = 0;
    const auto &it = counters.counters->histograms.find(internedHistogram);
    if (it != counters.counters->histograms.end()) {
        const auto &map = it->second;
        for (const auto &entry : map) {
            rv += entry.second;
        }
    }
    return rv;
}

vector<unique_ptr<CounterImpl::Timing>>
CounterStateDatabase::getTimings(ConstExprStr counter, vector<pair<ConstExprStr, ConstExprStr>> tags) const {
    // Note: Timers don't have interned names.
    vector<unique_ptr<CounterImpl::Timing>> rv;
    for (const auto &timing : counters.counters->timings) {
        auto timing_tags_size = timing.tags == nullptr ? 0 : timing.tags->size();
        if (strncmp(timing.measure, counter.str, counter.size + 1) == 0 && timing_tags_size >= tags.size()) {
            UnorderedMap<std::string, const char *> timingTags;
            if (timing.tags != nullptr) {
                for (const auto &tag : *timing.tags) {
                    timingTags[tag.first] = tag.second;
                }
            }

            int tagsMatched = 0;
            for (const auto &tag : tags) {
                auto it = timingTags.find(tag.first.str);
                if (it == timingTags.end()) {
                    break;
                }
                if (strncmp(it->second, tag.second.str, tag.second.size + 1) != 0) {
                    break;
                }
                tagsMatched++;
            }

            if (tagsMatched == tags.size()) {
                unique_ptr<vector<pair<char const *, string>>> copiedArgs;
                if (timing.args != nullptr) {
                    copiedArgs = make_unique<vector<pair<char const *, string>>>(*timing.args);
                }

                unique_ptr<vector<pair<char const *, char const *>>> copiedTags;
                if (timing.tags != nullptr) {
                    copiedTags = make_unique<vector<pair<char const *, char const *>>>(*timing.tags);
                }

                CounterImpl::Timing copied{timing.id,        timing.measure,  timing.start,
                                           timing.end,       timing.threadId, move(copiedArgs),
                                           move(copiedTags), timing.self,     timing.prev};
                rv.emplace_back(make_unique<CounterImpl::Timing>(move(copied)));
            }
        }
    }
    return rv;
}

void ProtocolTest::resetState() {
    fs = make_shared<MockFileSystem>(rootPath);
    diagnostics.clear();
    sourceFileContents.clear();
    auto opts = make_shared<realmain::options::Options>();
    opts->disableWatchman = true;
    if (useCache) {
        // Only recreate the cacheDir if we haven't created one before.
        if (cacheDir.empty()) {
            cacheDir = absl::StripAsciiWhitespace(exec("mktemp -d"));
        }
        opts->cacheDir = cacheDir;
    }

    if (useMultithreading) {
        lspWrapper = MultiThreadedLSPWrapper::create(rootPath, opts);
    } else {
        lspWrapper = SingleThreadedLSPWrapper::create(rootPath, opts);
    }
    lspWrapper->opts->fs = fs;
    lspWrapper->enableAllExperimentalFeatures();
}

ProtocolTest::ProtocolTest(bool useMultithreading, bool useCache)
    : useMultithreading(useMultithreading), useCache(useCache),
      rootPath("/Users/jvilk/stripe/pay-server"), // std::filesystem::canonical(".")),
      rootUri(fmt::format("file://{}", rootPath)) {
    resetState();
}

ProtocolTest::~ProtocolTest() {
    if (!cacheDir.empty()) {
        // Shut down lspwrapper before cleaning up database on disk.
        lspWrapper = nullptr;
        exec(fmt::format("rm -r {}", cacheDir));
    }
}

vector<unique_ptr<LSPMessage>> ProtocolTest::initializeLSP(bool supportsMarkdown,
                                                           optional<unique_ptr<SorbetInitializationOptions>> opts) {
    auto responses = sorbet::test::initializeLSP(rootPath, rootUri, *lspWrapper, nextId, supportsMarkdown, move(opts));
    updateDiagnostics(responses);
    return responses;
}

string ProtocolTest::fullPath(std::string_view path) const {
    return absl::StrCat(rootPath, "/", path);
}

string ProtocolTest::getUri(string_view filePath) {
    string fp = fullPath(filePath);
    string uri = filePathToUri(lspWrapper->config(), fp);
    return !absl::StartsWith(uri, "sorbet:") ? uri : absl::StrCat("file://", fp);
}

unique_ptr<LSPMessage> ProtocolTest::openFile(string_view path, string_view contents) {
    sourceFileContents[fullPath(path)] =
        make_shared<core::File>(fullPath(path), string(contents), core::File::Type::Normal);
    return makeOpen(getUri(path), contents, 1);
}

unique_ptr<LSPMessage> ProtocolTest::closeFile(string_view path) {
    // File is closed, so update contents from mock FS.
    try {
        sourceFileContents[fullPath(path)] =
            make_shared<core::File>(fullPath(path), string(fs->readFile(path)), core::File::Type::Normal);
    } catch (FileNotFoundException e) {
        auto it = sourceFileContents.find(path);
        if (it != sourceFileContents.end()) {
            sourceFileContents.erase(it);
        }
    }
    return makeClose(getUri(path));
}

unique_ptr<LSPMessage> ProtocolTest::changeFile(string_view path, string_view newContents, int version,
                                                bool cancellationExpected, int preemptionsExpected) {
    sourceFileContents[fullPath(path)] =
        make_shared<core::File>(fullPath(path), string(newContents), core::File::Type::Normal);
    return makeChange(getUri(path), newContents, version, cancellationExpected, preemptionsExpected);
}

unique_ptr<LSPMessage> ProtocolTest::documentSymbol(string_view path) {
    auto docSymParams = make_unique<DocumentSymbolParams>(make_unique<TextDocumentIdentifier>(getUri(path)));
    auto req = make_unique<RequestMessage>("2.0", nextId++, LSPMethod::TextDocumentDocumentSymbol, move(docSymParams));
    return make_unique<LSPMessage>(move(req));
}

unique_ptr<LSPMessage> ProtocolTest::workspaceSymbol(string_view query) {
    return makeWorkspaceSymbolRequest(nextId++, query);
}

unique_ptr<LSPMessage> ProtocolTest::hover(string_view path, int line, int character) {
    return makeHover(nextId++, getUri(path), line, character);
}

unique_ptr<LSPMessage> ProtocolTest::getDefinition(string_view path, int line, int character) {
    return makeDefinitionRequest(nextId++, getUri(path), line, character);
}

unique_ptr<LSPMessage> ProtocolTest::watchmanFileUpdate(vector<string> updatedFilePaths) {
    auto req = make_unique<NotificationMessage>("2.0", LSPMethod::SorbetWatchmanFileChange,
                                                make_unique<WatchmanQueryResponse>("", "", false, updatedFilePaths));
    return make_unique<LSPMessage>(move(req));
}

void ProtocolTest::writeFilesToFS(vector<pair<string, string>> files) {
    for (auto &file : files) {
        sourceFileContents[fullPath(file.first)] =
            make_shared<core::File>(fullPath(file.first), string(file.second), core::File::Type::Normal);
    }
    fs->writeFiles(files);
}

void ProtocolTest::deleteFileFromFS(string_view filename) {
    auto it = sourceFileContents.find(filename);
    if (it != sourceFileContents.end()) {
        sourceFileContents.erase(it);
    }
    fs->deleteFile(filename);
}

unique_ptr<LSPMessage> ProtocolTest::cancelRequest(int id) {
    return make_unique<LSPMessage>(
        make_unique<NotificationMessage>("2.0", LSPMethod::$CancelRequest, make_unique<CancelParams>(id)));
}

// Verify that messages are sound (contains proper JSON shape for method type) by serializing and re-parsing them.
vector<unique_ptr<LSPMessage>> verify(const vector<unique_ptr<LSPMessage>> &msgs) {
    vector<unique_ptr<LSPMessage>> reparsedMessages;
    for (auto &msg : msgs) {
        reparsedMessages.push_back(LSPMessage::fromClient(msg->toJSON()));
    }
    return reparsedMessages;
}

std::vector<std::unique_ptr<LSPMessage>> ProtocolTest::sendRaw(const std::string &json) {
    auto responses = verify(getLSPResponsesFor(*lspWrapper, LSPMessage::fromClient(json)));
    updateDiagnostics(responses);
    return responses;
}

vector<unique_ptr<LSPMessage>> ProtocolTest::send(const LSPMessage &message) {
    // Verify that message is sound (contains proper JSON shape for method type) by serializing and re-parsing it.
    return sendRaw(message.toJSON());
}

vector<unique_ptr<LSPMessage>> ProtocolTest::send(vector<unique_ptr<LSPMessage>> messages) {
    vector<unique_ptr<LSPMessage>> reparsedMessages = verify(messages);
    auto responses = verify(getLSPResponsesFor(*lspWrapper, move(reparsedMessages)));
    updateDiagnostics(responses);
    return responses;
}

void ProtocolTest::sendAsyncRaw(const string &json) {
    auto &wrapper = dynamic_cast<MultiThreadedLSPWrapper &>(*lspWrapper);
    wrapper.send(json);
}

void ProtocolTest::sendAsync(const LSPMessage &message) {
    sendAsyncRaw(message.toJSON());
}

unique_ptr<LSPMessage> ProtocolTest::readAsync() {
    auto &wrapper = dynamic_cast<MultiThreadedLSPWrapper &>(*lspWrapper);
    auto msg = wrapper.read(20000);
    if (msg) {
        updateDiagnostics(*msg);
    } else {
        FAIL_CHECK("Timeout waiting for LSP response.");
    }
    return msg;
}

void ProtocolTest::updateDiagnostics(const LSPMessage &msg) {
    if (msg.isNotification() && msg.method() == LSPMethod::TextDocumentPublishDiagnostics) {
        if (auto diagnosticParams = getPublishDiagnosticParams(msg.asNotification())) {
            // Will explicitly overwrite older diagnostics that are irrelevant.
            vector<unique_ptr<Diagnostic>> diagnostics;
            for (const auto &d : (*diagnosticParams)->diagnostics) {
                diagnostics.push_back(d->copy());
            }
            this->diagnostics[uriToFilePath(lspWrapper->config(), (*diagnosticParams)->uri)] = move(diagnostics);
        }
    }
}

void ProtocolTest::updateDiagnostics(const vector<unique_ptr<LSPMessage>> &messages) {
    for (auto &msg : messages) {
        updateDiagnostics(*msg);
    }
}

std::string ProtocolTest::readFile(std::string_view uri) {
    auto readFileResponses = send(LSPMessage(make_unique<RequestMessage>(
        "2.0", nextId++, LSPMethod::SorbetReadFile, make_unique<TextDocumentIdentifier>(string(uri)))));
    CHECK_EQ(readFileResponses.size(), 1);
    if (readFileResponses.size() == 1) {
        auto &readFileResponse = readFileResponses.at(0);
        CHECK(readFileResponse->isResponse());
        auto &readFileResult = get<unique_ptr<TextDocumentItem>>(*readFileResponse->asResponse().result);
        return readFileResult->text;
    }
    return "";
}

vector<unique_ptr<Location>> ProtocolTest::getDefinitions(std::string_view path, int line, int character) {
    auto defResponses = send(*getDefinition(path, line, character));
    CHECK_EQ(defResponses.size(), 1);
    if (defResponses.size() == 1) {
        auto &defResponse = defResponses.at(0);
        CHECK(defResponse->isResponse());
        auto &defResult = get<variant<JSONNullObject, vector<unique_ptr<Location>>>>(*defResponse->asResponse().result);
        return move(get<vector<unique_ptr<Location>>>(defResult));
    }
    return {};
}

void ProtocolTest::assertDiagnostics(vector<unique_ptr<LSPMessage>> messages, vector<ExpectedDiagnostic> expected) {
    for (auto &msg : messages) {
        // Ignore typecheck run and sorbet/fence messages. They do not impact semantics.
        if (!isTypecheckRun(*msg) && !isSorbetFence(*msg)) {
            assertNotificationMessage(LSPMethod::TextDocumentPublishDiagnostics, *msg);
        }
    }

    // Convert ExpectedDiagnostic into ErrorAssertion objects.
    vector<shared_ptr<ErrorAssertion>> errorAssertions;
    for (auto e : expected) {
        auto range = RangeAssertion::makeRange(e.line);
        errorAssertions.push_back(ErrorAssertion::make(fullPath(e.path), range, e.line, e.message, "error"));
    }

    // Use same logic as main test runner.
    ErrorAssertion::checkAll(sourceFileContents, errorAssertions, diagnostics);
}

const CounterStateDatabase ProtocolTest::getCounters() {
    auto results = getLSPResponsesFor(*lspWrapper, make_unique<LSPMessage>(make_unique<RequestMessage>(
                                                       "2.0", nextId++, LSPMethod::GETCOUNTERS, nullopt)));
    CHECK_EQ(results.size(), 1);
    auto &result = results.at(0);
    CHECK(result->isResponse());
    auto &response = result->asResponse();
    auto &counters = get<unique_ptr<SorbetCounters>>(response.result.value());
    return CounterStateDatabase(move(counters->counters));
}

} // namespace sorbet::test::lsp
