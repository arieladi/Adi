// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/control/consent.hpp"
#include "adi/control/consent_file.hpp"
#include "adi/control/consent_scope.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;
using namespace adi::control;

constexpr std::int64_t kNow = 1795780000;
int failures = 0;
int checks = 0;

void check(bool result, std::string_view what) {
    ++checks;
    if (!result) {
        ++failures;
        std::printf("  FAIL  %.*s\n", static_cast<int>(what.size()), what.data());
    }
}

struct LedgerFixture {
    adi::test::TempDirectory temp;
    fs::path path;
    ConsentLedger ledger;

    explicit LedgerFixture(const char* name)
        : temp("consent", name), path(temp.path() / "control" / "consent.json"), ledger(path) {}
};

ConsentRequest readRequest() {
    return {"shared-project", {"paired-client", "https://provider.example.test/v1", false},
            ConsentCapability::ProjectRead, {{ConsentData::Parameters, 42}}};
}

void testScopeIsolation() {
    std::puts("[consent scope follows the project, connection, destination and capability]");
    LedgerFixture fixture("scope");
    const auto request = readRequest();
    check(fixture.ledger.authorize(request, kNow) == ConsentDecision::ConsentRequired,
          "parameters alone need consent");
    check(fixture.ledger.grant(request, ConsentTerm::UntilRevoked, kNow),
          "a persistent grant can be recorded locally");
    ConsentLedger reopened(fixture.path);
    check(reopened.authorize(request, kNow) == ConsentDecision::Allowed,
          "an exact match still works when the ledger is reopened");

    auto different = request;
    different.projectUuid = "different-project";
    check(reopened.authorize(different, kNow) == ConsentDecision::ConsentRequired,
          "the same recipient needs consent for a different project");
    different = request;
    different.destination.connection = "other-pairing";
    check(reopened.authorize(different, kNow) == ConsentDecision::ConsentRequired,
          "the same recipient needs consent for a different connection");
    different = request;
    different.destination.endpoint = "https://provider2.example.test/v1";
    check(reopened.authorize(different, kNow) == ConsentDecision::ConsentRequired,
          "the same connection needs consent for a different recipient");
    different = request;
    different.destination.externalMcp = true;
    check(reopened.authorize(different, kNow) == ConsentDecision::ConsentRequired,
          "an external MCP host does not inherit a direct connection's grant");
    different = request;
    different.capability = ConsentCapability::ModelProvider;
    check(reopened.authorize(different, kNow) == ConsentDecision::ConsentRequired,
          "a project read grant does not authorize a model provider");
    different = request;
    different.data = {{ConsentData::Notes, 42}};
    check(reopened.authorize(different, kNow) == ConsentDecision::ConsentRequired,
          "the same byte count does not grant a different data kind");

    check(reopened.revoke(request) && fixture.ledger.authorize(request, kNow) == ConsentDecision::ConsentRequired,
          "revocation takes effect across ledger instances");
}

void testOnce() {
    std::puts("[this-time grants consume only an authorized full disclosure]");
    LedgerFixture fixture("once");
    const auto parameters = readRequest();
    auto audio = parameters;
    audio.data = {{ConsentData::Audio, 125}};
    auto both = parameters;
    both.data.push_back(audio.data.front());

    check(fixture.ledger.grant(parameters, ConsentTerm::ThisTime, kNow), "grant parameter disclosure once");
    check(fixture.ledger.authorize(both, kNow) == ConsentDecision::ConsentRequired,
          "parameters alone cannot authorize a disclosure with audio");
    check(fixture.ledger.grant(audio, ConsentTerm::ThisTime, kNow), "grant audio disclosure once");
    check(fixture.ledger.authorize(both, kNow) == ConsentDecision::ConsentRequired,
          "separate approvals cannot be combined into a new disclosure");
    check(fixture.ledger.grant(both, ConsentTerm::ThisTime, kNow), "approve the combined disclosure");
    check(fixture.ledger.authorize(parameters, kNow) == ConsentDecision::ConsentRequired,
          "a subset cannot spend part of a one-time approval");
    auto wrongBytes = both;
    wrongBytes.data.back().bytes = 126;
    check(fixture.ledger.authorize(wrongBytes, kNow) == ConsentDecision::ConsentRequired,
          "one extra byte requires new consent");
    wrongBytes.data.back().bytes = 124;
    check(fixture.ledger.authorize(wrongBytes, kNow) == ConsentDecision::ConsentRequired,
          "a smaller once disclosure must also match its exact approved byte count");
    auto wrongPayload = both;
    wrongPayload.disclosureFingerprint = "another-payload-with-the-same-byte-counts";
    check(fixture.ledger.authorize(wrongPayload, kNow) == ConsentDecision::ConsentRequired,
          "matching sizes cannot substitute another payload fingerprint");
    check(fixture.ledger.authorize(both, kNow) == ConsentDecision::Allowed,
          "none of the refusals consumed the approved disclosure");
    ConsentLedger reopened(fixture.path);
    check(reopened.authorize(parameters, kNow) == ConsentDecision::ConsentRequired &&
              reopened.authorize(audio, kNow) == ConsentDecision::ConsentRequired,
          "the successful disclosure consumed both grants durably");
    check(!reopened.grant(ConsentRequest{}, ConsentTerm::ThisTime, kNow),
          "an empty disclosure cannot create a grant");
}

void testExpiry() {
    std::puts("[each data kind expires independently]");
    LedgerFixture fixture("expiry");
    const auto parameters = readRequest();
    auto notes = parameters;
    notes.data = {{ConsentData::Notes, 157}};
    auto both = parameters;
    both.data.push_back(notes.data.front());
    check(fixture.ledger.grant(parameters, ConsentTerm::OneHour, kNow) &&
              fixture.ledger.grant(notes, ConsentTerm::OneDay, kNow),
          "record different expiry terms in the same scope");
    ConsentLedger reader(fixture.path);
    check(reader.authorize(both, kNow + 3599) == ConsentDecision::Allowed,
          "both grants apply until their respective expiry");
    check(reader.authorize(both, kNow + 3600) == ConsentDecision::ConsentRequired &&
              reader.authorize(notes, kNow + 3600) == ConsentDecision::Allowed,
          "at the hour boundary only the parameter grant expires");
    check(reader.authorize(notes, kNow + 86399) == ConsentDecision::Allowed &&
              reader.authorize(notes, kNow + 86400) == ConsentDecision::ConsentRequired,
          "the day grant expires at exactly one day");
}

void overwrite(const fs::path& file, const std::string& bytes) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
}

std::string contents(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void testBadLocalFiles() {
    std::puts("[unavailable ledger fails closed]");
    LedgerFixture fixture("files");
    const auto request = readRequest();
    const bool granted = fixture.ledger.grant(request, ConsentTerm::UntilRevoked, kNow);
    check(granted, "seed a valid private ledger");
    if (!granted) return;
    const auto original = contents(fixture.path);
    check(!original.empty(), "ledger was written");
    overwrite(fixture.path, "{\"version\":0," + original.substr(1));
    check(fixture.ledger.authorize(request, kNow) == ConsentDecision::Unavailable,
          "an ambiguous document with a duplicate key cannot grant consent");
    overwrite(fixture.path, original);
    const auto locked = adi::control::detail::withConsentFileLock(fixture.path, [&] {
        ConsentLedger other(fixture.path);
        check(other.authorize(request, kNow) == ConsentDecision::Unavailable,
              "a held file lock cannot be bypassed");
        check(!other.grant(request, ConsentTerm::OneHour, kNow), "a held lock cannot be overwritten");
    });
    check(locked && fixture.ledger.authorize(request, kNow) == ConsentDecision::Allowed,
          "regular authorization resumes after releasing the lock");

    // Truncation of the existing fixture preserves the private access mode.
    const std::string oversized(ConsentLedger::kMaxFileBytes + 1, ' ');
    overwrite(fixture.path, oversized);
    check(fixture.ledger.authorize(request, kNow) == ConsentDecision::Unavailable &&
              !fixture.ledger.grant(request, ConsentTerm::UntilRevoked, kNow) &&
              fs::file_size(fixture.path) == oversized.size(),
          "an oversized ledger authorizes nothing and is not replaced");
    const std::string malformed = R"({"version":1,"grants":[{})";
    overwrite(fixture.path, malformed);
    check(fixture.ledger.authorize(request, kNow) == ConsentDecision::Unavailable &&
              !fixture.ledger.grant(request, ConsentTerm::UntilRevoked, kNow) &&
              contents(fixture.path) == malformed,
          "a malformed ledger authorizes nothing and keeps its bytes");
    fs::remove(fixture.path);
    fs::create_directory(fixture.path);
    check(fixture.ledger.authorize(request, kNow) == ConsentDecision::Unavailable &&
              !fixture.ledger.grant(request, ConsentTerm::UntilRevoked, kNow),
          "a path that cannot be read as a ledger fails closed");
}

void testLocality() {
    std::puts("[only unambiguous loopback bypasses disclosure]");
    for (const std::string_view endpoint : {"localhost", "HTTPS://LOCALHOST:9999/v1", "http://127.0.0.1",
                                            "ws://127.4.5.6:1000/rpc", "http://[::1]:11434/v1",
                                            "[0:0:0:0:0:0:0:0001]", "0::1"}) {
        check(isLoopbackDestination(endpoint), endpoint);
    }
    for (const std::string_view endpoint : {"", "https://localhost.example.test", "http://localhost@192.168.1.1",
                                            "http://192.168.1.1@localhost", "http://192.168.1.5",
                                            "http://127.0.0.1.example.test", "http://localhost%2e",
                                            "http://127.1", "http://0177.0.0.1", "http://0x7f000001",
                                            "https://localhost:99999", "http://localhost\\@example.test",
                                            "http://localhost\t", "https://[::1]example.test", "[:::1]",
                                            "http://::1", "http://[2001:db8::1]", "unix://localhost"}) {
        check(!isLoopbackDestination(endpoint), endpoint);
    }
    LedgerFixture fixture("local");
    auto request = readRequest();
    request.destination.endpoint = "http://[::1]:11434/v1";
    check(fixture.ledger.authorize(request, kNow) == ConsentDecision::Allowed,
          "the directly connected loopback service needs no grant");
    request.destination.externalMcp = true;
    check(fixture.ledger.authorize(request, kNow) == ConsentDecision::ConsentRequired,
          "a loopback external MCP host needs a per-project grant");
    request.destination.externalMcp = false;
    request.destination.endpoint = "http://192.168.1.5:11434/v1";
    check(fixture.ledger.authorize(request, kNow) == ConsentDecision::ConsentRequired,
          "a privately addressed LAN service still needs consent");
}

void testPluginServerBinding() {
    std::puts("[a plug-in server grant stays with the original project file]");
    LedgerFixture fixture("server");
    const auto originalPath = fixture.temp.path() / "original.adi";
    const auto copyPath = fixture.temp.path() / "ordinary-copy.adi";
    adi::StoreError error{};
    auto original = adi::Store::create(originalPath, error);
    check(original != nullptr, "create the original project");
    if (!original) return;

    ConsentRequest request{original->projectUuid(), {"server-connection", "http://localhost:55000", false},
                           ConsentCapability::PluginHosting, {{ConsentData::PluginState, 85}},
                           projectFileConsentBinding(*original)};
    if (request.fileBinding.empty()) {
        check(!fixture.ledger.grant(request, ConsentTerm::UntilRevoked, kNow) &&
                  fixture.ledger.authorize(request, kNow) == ConsentDecision::Unavailable,
              "a filesystem with no durable file binding cannot carry a server grant");
        return;
    }
    check(fixture.ledger.authorize(request, kNow) == ConsentDecision::ConsentRequired,
          "a loopback plug-in server still needs consent");
    check(fixture.ledger.grant(request, ConsentTerm::UntilRevoked, kNow), "grant to one project and server");
    check(fixture.ledger.authorize(request, kNow) == ConsentDecision::Allowed, "the original grant works");
    auto differentServer = request;
    differentServer.destination.endpoint = "http://localhost:55001";
    check(fixture.ledger.authorize(differentServer, kNow) == ConsentDecision::ConsentRequired,
          "a different plug-in server needs its own grant");

    const auto copied = original->saveCopy(copyPath, adi::StoreFileCollision::Fail, adi::StoreCopyPolicy::preserve);
    check(copied.ok, "save a copy that preserves the original project's UUID");
    if (!copied.ok) return;
    auto copy = adi::Store::open(copyPath, error, true);
    check(copy && copy->projectUuid() == request.projectUuid, "the preserved copy keeps the project UUID");
    if (!copy) return;
    auto copyRequest = request;
    copyRequest.fileBinding = projectFileConsentBinding(*copy);
    check(!copyRequest.fileBinding.empty() && copyRequest.fileBinding != request.fileBinding &&
              fixture.ledger.authorize(copyRequest, kNow) == ConsentDecision::ConsentRequired,
          "the copy's file binding prevents reuse of the original server grant");

    original.reset();
    original = adi::Store::open(originalPath, error, true);
    check(original && projectFileConsentBinding(*original) == request.fileBinding,
          "the original file binding survives reopening");
    ConsentLedger reopened(fixture.path);
    check(reopened.authorize(request, kNow) == ConsentDecision::Allowed,
          "reopening the original and the local ledger preserves its grant");
}

}  // namespace

int main() {
    try {
        testScopeIsolation();
        testOnce();
        testExpiry();
        testBadLocalFiles();
        testLocality();
        testPluginServerBinding();
    } catch (const std::exception& e) {
        check(false, e.what());
    }
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
