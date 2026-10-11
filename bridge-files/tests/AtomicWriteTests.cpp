// Order of steps in an atomic replace, against a fake file system.

#include <TestSupport.hpp>

#include <FileBridgeAtomicWrite.hpp>

#include <map>
#include <string>
#include <vector>

using namespace UE4SSLuaFileBridge::Core;
using UE4SSLuaFileBridge::NativeContract::ErrorCode;

namespace
{
class FakeOps final : public AtomicReplaceOps
{
public:
    std::map<std::string, std::string> files;
    std::vector<std::string> steps;
    std::string content = "new";
    std::string fail_step;
    uint32_t replace_error = 0; // simulated ReplaceFileW error, 0 = success
    int rename_failures = 0;    // rename_new fails this many times first
    int pauses = 0;

    Status fill_temp(const std::string& temp) override
    {
        steps.push_back("fill " + temp);
        if (fail_step == "fill")
        {
            files[temp] = "partial";
            return fail(ErrorCode::io, "disk full");
        }
        files[temp] = content;
        return Done{};
    }
    Outcome<bool> target_exists(const std::string& target) override
    {
        steps.push_back("exists " + target);
        return files.count(target) != 0;
    }
    ReplaceResult replace(const std::string& target, const std::string& temp, const std::string& backup) override
    {
        steps.push_back("replace " + target + " " + temp + " " + backup);
        if (fail_step == "replace") return ReplaceResult{fail(ErrorCode::busy, "sharing violation"), ReplaceFault::unchanged};
        if (replace_error != 0)
        {
            const auto fault = classify_replace_error(replace_error, !backup.empty());
            if (fault == ReplaceFault::target_free)
            {
                // The old content is gone from the target name (to the backup, if any).
                if (!backup.empty()) files[backup] = files[target];
                files.erase(target);
            }
            return ReplaceResult{fail(ErrorCode::io, "Replace T (win32 " + std::to_string(replace_error) + ")"), fault};
        }
        if (!backup.empty()) files[backup] = files[target];
        files[target] = files[temp];
        files.erase(temp);
        return ReplaceResult{};
    }
    void pause() override
    {
        ++pauses;
        steps.push_back("pause");
    }
    Status rename_new(const std::string& temp, const std::string& target) override
    {
        steps.push_back("rename " + temp + " " + target);
        if (fail_step == "rename") return fail(ErrorCode::exists, "raced");
        if (rename_failures > 0)
        {
            --rename_failures;
            return fail(ErrorCode::busy, "temporary file in use");
        }
        files[target] = files[temp];
        files.erase(temp);
        return Done{};
    }
    void discard(const std::string& temp) override
    {
        steps.push_back("discard " + temp);
        files.erase(temp);
    }
};

void names()
{
    CHECK(temp_sibling("C:/a/x.sav", 42, 7) == "C:/a/x.sav.xbtmp-42-7");
    CHECK(backup_sibling("C:/a/x.sav") == "C:/a/x.sav.bak");
}

void new_file()
{
    FakeOps ops;
    CHECK(atomic_replace(ops, "T", "T.tmp", "", true).ok());
    CHECK((ops.steps == std::vector<std::string>{"fill T.tmp", "exists T", "rename T.tmp T"}));
    CHECK(ops.files.size() == 1 && ops.files["T"] == "new");
}

void replace_existing_with_backup()
{
    FakeOps ops;
    ops.files["T"] = "old";
    CHECK(atomic_replace(ops, "T", "T.tmp", "T.bak", true).ok());
    CHECK((ops.steps == std::vector<std::string>{"fill T.tmp", "exists T", "replace T T.tmp T.bak"}));
    CHECK(ops.files["T"] == "new" && ops.files["T.bak"] == "old" && !ops.files.count("T.tmp"));
}

void refuse_existing_without_overwrite()
{
    FakeOps ops;
    ops.files["T"] = "old";
    auto status = atomic_replace(ops, "T", "T.tmp", "", false);
    CHECK(!status && status.failure().code == ErrorCode::exists);
    CHECK(ops.files["T"] == "old" && !ops.files.count("T.tmp"));
    CHECK(ops.steps.back() == "discard T.tmp");
}

void failures_leave_target_unchanged()
{
    for (const char* step : {"fill", "replace"})
    {
        FakeOps ops;
        ops.files["T"] = "old";
        ops.fail_step = step;
        auto status = atomic_replace(ops, "T", "T.tmp", "", true);
        CHECK(!status);
        CHECK(ops.files["T"] == "old");
        CHECK(!ops.files.count("T.tmp"));
        CHECK(ops.steps.back() == "discard T.tmp");
    }
    FakeOps ops;
    ops.fail_step = "rename";
    auto status = atomic_replace(ops, "T", "T.tmp", "", true);
    CHECK(!status && status.failure().code == ErrorCode::exists);
    CHECK(!ops.files.count("T") && !ops.files.count("T.tmp"));
}
}

void classification()
{
    CHECK(classify_replace_error(1175, false) == ReplaceFault::unchanged);
    CHECK(classify_replace_error(1175, true) == ReplaceFault::unchanged);
    CHECK(classify_replace_error(1176, false) == ReplaceFault::target_free);
    CHECK(classify_replace_error(1176, true) == ReplaceFault::unchanged);
    CHECK(classify_replace_error(1177, false) == ReplaceFault::target_free);
    CHECK(classify_replace_error(1177, true) == ReplaceFault::target_free);
    CHECK(classify_replace_error(32, false) == ReplaceFault::unchanged);
}

bool discarded(const FakeOps& ops)
{
    for (const auto& step : ops.steps)
    {
        if (step.rfind("discard", 0) == 0) return true;
    }
    return false;
}

void unable_to_remove_replaced()
{
    // 1175: nothing changed, so the temporary file is discarded.
    for (const char* backup : {"", "T.bak"})
    {
        FakeOps ops;
        ops.files["T"] = "old";
        ops.replace_error = 1175;
        auto status = atomic_replace(ops, "T", "T.tmp", backup, true);
        CHECK(!status);
        CHECK(ops.files["T"] == "old" && !ops.files.count("T.tmp"));
        CHECK(discarded(ops));
    }
}

void unable_to_move_replacement()
{
    // 1176 without a backup: target gone, new content only in the temporary file.
    FakeOps ops;
    ops.files["T"] = "old";
    ops.replace_error = 1176;
    CHECK(atomic_replace(ops, "T", "T.tmp", "", true).ok());
    CHECK(!discarded(ops));
    CHECK(ops.files["T"] == "new" && !ops.files.count("T.tmp"));
    CHECK(ops.steps.back() == "rename T.tmp T");

    // The rename keeps failing: keep the temporary file and name it.
    FakeOps stuck;
    stuck.files["T"] = "old";
    stuck.replace_error = 1176;
    stuck.rename_failures = 99;
    auto status = atomic_replace(stuck, "T", "T.tmp", "", true);
    CHECK(!status && status.failure().code == ErrorCode::io);
    CHECK(!discarded(stuck));
    CHECK(stuck.files["T.tmp"] == "new");
    CHECK(status.failure().message.find("T.tmp") != std::string::npos);
    CHECK(stuck.pauses == rename_attempts_after_partial_replace - 1);

    // 1176 with a backup: both files keep their names, so discarding is safe.
    FakeOps backed;
    backed.files["T"] = "old";
    backed.replace_error = 1176;
    CHECK(!atomic_replace(backed, "T", "T.tmp", "T.bak", true));
    CHECK(discarded(backed) && backed.files["T"] == "old");
}

void unable_to_move_replacement_2()
{
    // 1177: old content moved to the backup name, new content in the temporary file.
    FakeOps ops;
    ops.files["T"] = "old";
    ops.replace_error = 1177;
    ops.rename_failures = 1; // succeeds on the second attempt
    CHECK(atomic_replace(ops, "T", "T.tmp", "T.bak", true).ok());
    CHECK(!discarded(ops));
    CHECK(ops.files["T"] == "new" && ops.files["T.bak"] == "old" && !ops.files.count("T.tmp"));
    CHECK(ops.pauses == 1);

    FakeOps stuck;
    stuck.files["T"] = "old";
    stuck.replace_error = 1177;
    stuck.rename_failures = 99;
    auto status = atomic_replace(stuck, "T", "T.tmp", "T.bak", true);
    CHECK(!status && status.failure().code == ErrorCode::io);
    CHECK(!discarded(stuck));
    CHECK(stuck.files["T.tmp"] == "new" && stuck.files["T.bak"] == "old");
    CHECK(status.failure().message.find("T.bak") != std::string::npos);
}

void leftover_names()
{
    CHECK(temp_sibling_pid("a.sav.xbtmp-42-7") == 42u);
    CHECK(temp_sibling_pid(temp_sibling("C:/x/y.json", 4000000000u, 99)) == 4000000000u);
    CHECK(!temp_sibling_pid("a.sav"));
    CHECK(!temp_sibling_pid(".xbtmp-1-2"));
    CHECK(!temp_sibling_pid("a.xbtmp-1"));
    CHECK(!temp_sibling_pid("a.xbtmp-1-"));
    CHECK(!temp_sibling_pid("a.xbtmp-x-2"));
    CHECK(!temp_sibling_pid("a.xbtmp-99999999999-2"));
}

int main()
{
    classification();
    unable_to_remove_replaced();
    unable_to_move_replacement();
    unable_to_move_replacement_2();
    leftover_names();
    names();
    new_file();
    replace_existing_with_backup();
    refuse_existing_without_overwrite();
    failures_leave_target_unchanged();
    return LexTests::finish("AtomicWriteTests");
}
