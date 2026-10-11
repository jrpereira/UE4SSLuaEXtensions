// UE4SSLuaFileBridge: the UE4SS C++ mod. Owns one Session per Lua mod, the
// 26 natives of FileBridgeNativeContract.hpp, Tail dispatch and lifetimes.

#include <UE4SSLuaABI.hpp>

#include <EmbeddedLuaAPI.hpp>
#include <FileBridgeAtomicWrite.hpp>
#include <FileBridgeNativeContract.hpp>
#include <FileBridgePaths.hpp>
#include <FileBridgePolicy.hpp>
#include <FileBridgeRecords.hpp>
#include <FileBridgeText.hpp>
#include <LexImplementation.hpp>
#include <LexReport.hpp>
#include <Version.hpp>
#include <Win32Files.hpp>

#include <dispatch/DispatchBacklog.hpp>
#include <dispatch/DispatchBudget.hpp>
#include <dispatch/QueueDispatchSchedule.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace
{
using RC::LuaMadeSimple::Lua;
using UE4SSLuaFileBridge::Core::Access;
using UE4SSLuaFileBridge::Core::Done;
using UE4SSLuaFileBridge::Core::ErrorCode;
using UE4SSLuaFileBridge::Core::Failure;
using UE4SSLuaFileBridge::Core::Outcome;
using UE4SSLuaFileBridge::Core::Policy;
using UE4SSLuaFileBridge::Core::Status;
using UE4SSLuaFileBridge::Core::TargetKind;
namespace Contract = UE4SSLuaFileBridge::NativeContract;
namespace Core = UE4SSLuaFileBridge::Core;
namespace Win32 = UE4SSLuaFileBridge::Win32;

void report(std::string_view message)
{
    Lex::report("UE4SSLuaFileBridge", message);
}

// --- Results ---------------------------------------------------------------

using Value = std::variant<std::monostate, bool, int64_t, double, std::string>;

struct Reply
{
    std::vector<Value> values;
};

Reply failure(ErrorCode code, std::string message)
{
    return Reply{{std::monostate{}, std::string(Contract::to_string(code)), std::move(message)}};
}

Reply failure(const Failure& failed)
{
    return failure(failed.code, failed.message);
}

template<class... Values>
Reply success(Values&&... values)
{
    Reply reply;
    (reply.values.emplace_back(Value(std::forward<Values>(values))), ...);
    return reply;
}

// Runs after the native's try block: a Lua memory error while pushing (for
// example a ReadBytes result near max_read_bytes) is raised by Lua itself and
// can't be turned into `nil, code, message` here. Results are bounded by
// max_read_bytes; see "Known limitations" in FileBridgeNativeContract.hpp.
int push(const Lua& lua, const Reply& reply)
{
    for (const auto& value : reply.values)
    {
        if (std::holds_alternative<std::monostate>(value)) lua.set_nil();
        else if (const auto* flag = std::get_if<bool>(&value)) lua.set_bool(*flag);
        else if (const auto* integer = std::get_if<int64_t>(&value)) lua.set_integer(*integer);
        else if (const auto* number = std::get_if<double>(&value)) lua.set_number(*number);
        else lua.set_string(std::get<std::string>(value));
    }
    return static_cast<int>(reply.values.size());
}

// Arguments are consumed left to right (get_* remove argument 1).
class Call
{
public:
    explicit Call(const Lua& lua) : lua_(lua) {}
    int64_t integer() { return lua_.get_integer(1); }
    std::string string()
    {
        const auto view = lua_.get_string(1);
        return std::string(view.data() ? view : std::string_view{}); // copy before any other Lua call (R3)
    }

private:
    const Lua& lua_;
};

bool shape_matches(const Lua& lua, std::string_view shape)
{
    if (lua.get_stack_size() != static_cast<int32_t>(shape.size())) return false;
    for (std::size_t i = 0; i < shape.size(); ++i)
    {
        const auto index = static_cast<int32_t>(i + 1);
        if (shape[i] == 'i' ? !lua.is_integer(index) : !lua.is_string(index)) return false;
    }
    return true;
}

double seconds(int64_t filetime)
{
    return static_cast<double>(filetime - Core::filetime_unix_epoch) / 10000000.0;
}

// --- Sessions --------------------------------------------------------------

struct Subscription
{
    int64_t id{};
    int64_t token{};
    std::string path;
    std::unique_ptr<Win32::TailFile> file;
    std::atomic_bool open{true};
};

struct Session
{
    uint64_t id{};
    Lua* lua{};
    lua_State* state{};
    int32_t dispatcher_ref{};
    std::atomic_bool active{true};
    std::string mod_name;
    Core::LocationSet locations;
    int64_t safe_policy{};

    std::mutex mutex;
    std::unordered_map<int64_t, std::shared_ptr<const Policy>> policies;
    std::unordered_map<int64_t, std::shared_ptr<Win32::Stream>> streams;
    std::unordered_map<int64_t, std::shared_ptr<Subscription>> subscriptions;
    bool owner_refreshed{false};
};

struct Pending
{
    std::shared_ptr<Session> session;
    std::shared_ptr<Subscription> subscription;
    Contract::DispatchKind kind{Contract::DispatchKind::data};
    std::string text;
    int64_t offset{};
    int64_t flags{};
};

class FileBridgeMod;
FileBridgeMod* active_bridge{};

std::string session_gone()
{
    return "Lua session is not registered or is stopping";
}

class FileBridgeMod final : public RC::CppUserModBase
{
public:
    FileBridgeMod()
    {
        ModName = L"UE4SSLuaFileBridge";
        ModVersion = UE4SSLFB_WIDEN(UE4SSLFB_VERSION);
        ModDescription = L"File and folder access for UE4SS Lua mods, checked against access environments";
        ModAuthors = L"UE4SS Lua Extension Bridges contributors";
        ModIntendedSDKVersion = L"3.0.1-97b7e501";
        active_bridge = this;
        roots_ = Win32::discover_roots(&active_bridge);
        if (!roots_.ok)
        {
            report("file access unavailable: " + roots_.error);
        }
        else
        {
            report("game folder " + roots_.game + ", user folder " + roots_.user + ", mods folder " + roots_.mods);
            empty_temp_folders();
        }
    }

    ~FileBridgeMod() override
    {
        std::vector<std::shared_ptr<Session>> all;
        {
            std::scoped_lock lock(sessions_mutex_);
            for (auto& [_, session] : sessions_) all.push_back(session);
        }
        for (auto& session : all) close_session(*session);
        active_bridge = nullptr;
    }

    void on_update() override
    {
        if (!schedule_.due(UE4SSXB::QueueDispatchSchedule::Clock::now())) return;
        if (backlog_.empty()) collect_tail_deliveries();
        using Budget = UE4SSXB::DispatchBudget;
        const Budget budget(Budget::Clock::now(), max_deliveries_, std::chrono::microseconds(max_dispatch_us_));
        UE4SSXB::dispatch_budgeted(backlog_, budget, [&](Pending pending) { return deliver(pending); },
            [] { return Budget::Clock::now(); });
        if (!backlog_.empty()) budget_exhausted_passes_.fetch_add(1, std::memory_order_relaxed);
    }

    void on_lua_start(RC::StringViewType mod_name, Lua& lua, Lua& main_lua, Lua& async_lua, Lua* hook_lua) override
    {
        // Nothing may escape into UE4SS's loop over C++ mods (contract section 2,
        // step 5): log, publish nothing, and that mod has no UE4SSLuaFileBridge.
        try
        {
            start_session(mod_name, lua);
        }
        catch (const std::exception& error)
        {
            report_start_failure(mod_name, error.what());
        }
        catch (...)
        {
            report_start_failure(mod_name, "unknown exception");
        }
        (void)main_lua;
        (void)async_lua;
        (void)hook_lua;
    }

    void report_start_failure(RC::StringViewType mod_name, std::string_view what)
    {
        try
        {
            const auto name = Core::utf16_to_utf8(std::u16string(mod_name.begin(), mod_name.end()));
            report("starting the file bridge for mod \"" + (name ? *name : std::string("?")) + "\" failed: " +
                std::string(what));
        }
        catch (...)
        {
        }
    }

    void start_session(RC::StringViewType mod_name, Lua& lua)
    {
        auto session = std::make_shared<Session>();
        session->id = next_session_id_.fetch_add(1);
        session->lua = &lua;
        session->state = lua.get_lua_state();
        const auto name = Core::utf16_to_utf8(std::u16string(mod_name.begin(), mod_name.end()));
        session->mod_name = name ? *name : std::string{};
        if (roots_.ok)
        {
            session->locations = Core::make_locations(roots_.game, roots_.user, roots_.mods, session->mod_name);
        }
        else
        {
            session->locations = Core::make_locations({}, {}, {}, {});
        }
        session->safe_policy = next_policy_id_.fetch_add(1);
        auto safe = Core::safe_policy(session->locations);
        for (auto& grant : safe.grants)
        {
            if (auto real = resolver_.resolve(grant.lexical, true)) grant.real = std::move(real.value());
        }
        session->policies.emplace(session->safe_policy, std::make_shared<const Policy>(std::move(safe)));
        if (roots_.ok && !session->locations.bound("mod"))
        {
            report("mod \"" + session->mod_name + "\": its name isn't one folder name, so mod, moddata and temp are unbound");
        }

        register_natives(lua);
        lua.execute_string(std::string(Contract::session_global) + " = " + std::to_string(session->id));
        std::string api;
        for (const auto chunk : UE4SSLuaFileBridge::embedded_lua_api_chunks) api.append(chunk);
        lua.execute_string(api);
        // The chunk returns the dispatcher; keep one registry reference per session.
        session->dispatcher_ref = lua.registry().make_ref();

        std::scoped_lock lock(sessions_mutex_);
        sessions_.emplace(session->id, session);
        by_state_[session->state] = session->id;
    }

    void on_lua_stop(RC::StringViewType, Lua& lua, Lua&, Lua&, Lua*) override
    {
        std::shared_ptr<Session> session;
        {
            std::scoped_lock lock(sessions_mutex_);
            const auto found = by_state_.find(lua.get_lua_state());
            if (found == by_state_.end()) return;
            const auto record = sessions_.find(found->second);
            if (record != sessions_.end()) session = record->second;
            by_state_.erase(found);
        }
        if (!session) return;
        if (RC::Unreal::IsInGameThread() && session->active.load())
        {
            try
            {
                call_dispatcher(*session, Contract::DispatchKind::stop, 0, {}, 0, 0);
            }
            catch (const std::exception& error)
            {
                report(std::string("Lua-stop cleanup for mod \"") + session->mod_name + "\" failed: " + error.what());
            }
            catch (...)
            {
                report("Lua-stop cleanup for mod \"" + session->mod_name + "\" failed: unknown exception");
            }
        }
        close_session(*session);
        // The Session record stays until bridge destruction; ids are never reused.
    }

    // --- Native plumbing ---------------------------------------------------

    std::shared_ptr<Session> find_session(int64_t id)
    {
        std::scoped_lock lock(sessions_mutex_);
        const auto found = sessions_.find(static_cast<uint64_t>(id));
        if (found == sessions_.end() || !found->second->active.load()) return nullptr;
        return found->second;
    }

    std::shared_ptr<const Policy> find_policy(Session& session, int64_t id)
    {
        std::scoped_lock lock(session.mutex);
        const auto found = session.policies.find(id);
        return found == session.policies.end() ? nullptr : found->second;
    }

    [[nodiscard]] bool roots_ok() const { return roots_.ok; }
    [[nodiscard]] const std::string& roots_error() const { return roots_.error; }

    Core::CheckEnvironment environment(Session& session)
    {
        return Core::CheckEnvironment{session.locations, resolver_, &Win32::equal_fold, roots_.profile};
    }

    Win32::Resolver& resolver() { return resolver_; }

    int64_t next_policy_id() { return next_policy_id_.fetch_add(1); }
    int64_t next_stream_id() { return next_stream_id_.fetch_add(1); }
    int64_t next_subscription_id() { return next_subscription_id_.fetch_add(1); }

    int64_t active_subscriptions()
    {
        int64_t count = 0;
        std::scoped_lock lock(sessions_mutex_);
        for (auto& [_, session] : sessions_)
        {
            if (!session->active.load()) continue;
            std::scoped_lock session_lock(session->mutex);
            count += static_cast<int64_t>(session->subscriptions.size());
        }
        return count;
    }

    int64_t delivered() const { return static_cast<int64_t>(delivered_.load(std::memory_order_relaxed)); }
    int64_t exhausted_passes() const { return static_cast<int64_t>(budget_exhausted_passes_.load(std::memory_order_relaxed)); }

    // Creates moddata (with its .owner marker) and temp when `real` lies in them.
    Status prepare_moddata(Session& session, Core::Checker& checker, const std::string& real)
    {
        const auto* moddata = checker.location_real("moddata");
        if (!moddata || !Core::is_within(real, *moddata, &Win32::equal_fold)) return Done{};
        const auto* user = checker.location_real("user");
        if (!user || !Core::is_within(*moddata, *user, &Win32::equal_fold)) return Done{};
        // Create every missing folder from the user root down to moddata.
        const auto user_depth = Core::segment_count(*user);
        const auto segments = Core::split_segments(*moddata);
        std::vector<std::string> parts(segments.begin(), segments.begin() + static_cast<std::ptrdiff_t>(user_depth));
        for (std::size_t i = user_depth; i < segments.size(); ++i)
        {
            parts.emplace_back(segments[i]);
            auto made = Win32::create_directory(Core::join_segments(parts));
            if (!made) return made;
        }
        const auto owner = Core::child_path(*moddata, ".owner");
        bool refresh = false;
        {
            std::scoped_lock lock(session.mutex);
            refresh = !session.owner_refreshed;
            session.owner_refreshed = true;
        }
        // The marker is bookkeeping: failing to update it (for example another
        // session of the same mod writing it) is logged, never the caller's error.
        const auto exists = Win32::probe(owner);
        if (!exists)
        {
            report("updating " + owner + " failed: " + exists.failure().message);
        }
        else if (exists.value().kind == Win32::Kind::missing || refresh)
        {
            const auto now = Win32::utc_timestamp();
            std::string first = now;
            if (exists.value().kind == Win32::Kind::file)
            {
                auto previous = Win32::read(owner, 0, -1, 64 * 1024);
                if (previous)
                {
                    const auto& text = previous.value();
                    const auto at = text.find("first_write=");
                    if (at != std::string::npos)
                    {
                        const auto end = text.find('\n', at);
                        first = text.substr(at + 12, end == std::string::npos ? std::string::npos : end - at - 12);
                    }
                }
            }
            const std::string content = "folder=" + session.mod_name + "\nfirst_write=" + first + "\nlast_write=" + now +
                "\nbridge_version=" + UE4SSLFB_VERSION + "\n";
            auto written = Win32::write_atomic(owner, content, {});
            if (!written) report("updating " + owner + " failed: " + written.failure().message);
        }
        if (const auto* temp = checker.location_real("temp"); temp && Core::is_within(real, *temp, &Win32::equal_fold))
        {
            auto made = Win32::create_directory(*temp);
            if (!made) return made;
        }
        return Done{};
    }

private:
    Win32::Roots roots_;
    Win32::Resolver resolver_;
    std::mutex sessions_mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<Session>> sessions_;
    std::unordered_map<lua_State*, uint64_t> by_state_;
    std::atomic_uint64_t next_session_id_{1};
    std::atomic<int64_t> next_policy_id_{1};
    std::atomic<int64_t> next_stream_id_{1};
    std::atomic<int64_t> next_subscription_id_{1};

    UE4SSXB::QueueDispatchSchedule schedule_{UE4SSXB::default_queue_checks_per_second};
    UE4SSXB::DispatchBacklog<Pending> backlog_;
    const uint32_t max_deliveries_{Win32::environment_limit(
        "UE4SSLFB_MAX_EVENTS_PER_PASS", Contract::default_max_deliveries_per_pass, 1000000)};
    const uint32_t max_dispatch_us_{Win32::environment_limit(
        "UE4SSLFB_MAX_DISPATCH_US", Contract::default_max_dispatch_us, 1000000)};
    std::atomic_uint64_t delivered_{0};
    std::atomic_uint64_t budget_exhausted_passes_{0};
    int64_t last_polled_subscription_{0}; // on_update only

    void register_natives(Lua& lua);

    void empty_temp_folders()
    {
        const auto base = roots_.user + "/Saved/ModData";
        for (const auto& name : Win32::subfolders(base))
        {
            const auto folder = base + "/" + name;
            const auto owner = Win32::probe(folder + "/.owner");
            if (!owner || owner.value().kind != Win32::Kind::file) continue;
            const auto temp = folder + "/temp";
            const auto kind = Win32::probe(temp);
            if (!kind || kind.value().kind != Win32::Kind::directory || kind.value().link) continue;
            auto entries = Win32::enumerate_tree(temp);
            if (!entries)
            {
                report("emptying " + temp + " failed: " + entries.failure().message);
                continue;
            }
            auto list = std::move(entries.value());
            if (!list.empty()) list.pop_back(); // keep the temp folder itself
            auto removed = Win32::remove_tree_entries(list);
            if (!removed) report("emptying " + temp + " failed: " + removed.failure().message);
        }
        remove_stale_temp_siblings(base);
    }

    // Temporary siblings ("<name>.xbtmp-<pid>-<n>") left by an earlier process
    // that stopped mid-write, under ModData folders with an .owner marker.
    // Leftovers elsewhere (mod folders, savegames) are left alone in 1.0.1.
    void remove_stale_temp_siblings(const std::string& base)
    {
        const auto current = Win32::process_id();
        std::size_t removed = 0;
        for (const auto& name : Win32::subfolders(base))
        {
            const auto folder = base + "/" + name;
            const auto owner = Win32::probe(folder + "/.owner");
            if (!owner || owner.value().kind != Win32::Kind::file) continue;
            auto entries = Win32::enumerate_tree(folder);
            if (!entries) continue;
            std::vector<Win32::TreeEntry> stale;
            for (auto& entry : entries.value())
            {
                if (entry.directory) continue;
                const auto pid = Core::temp_sibling_pid(Core::leaf_name(entry.path));
                if (pid && *pid != current) stale.push_back(std::move(entry));
            }
            auto status = Win32::remove_tree_entries(stale);
            if (!status) report("removing leftover temporary files under " + folder + " failed: " + status.failure().message);
            else removed += stale.size();
        }
        if (removed != 0) report("removed " + std::to_string(removed) + " leftover temporary file(s) under " + base);
    }

    void close_session(Session& session)
    {
        session.active.store(false);
        std::unordered_map<int64_t, std::shared_ptr<Win32::Stream>> streams;
        std::unordered_map<int64_t, std::shared_ptr<Subscription>> subscriptions;
        {
            std::scoped_lock lock(session.mutex);
            streams.swap(session.streams);
            subscriptions.swap(session.subscriptions);
            session.policies.clear();
        }
        for (auto& [_, subscription] : subscriptions) subscription->open.store(false);
        for (auto& [_, stream] : streams)
        {
            auto closed = stream->close();
            if (!closed) report("closing a stream of mod \"" + session.mod_name + "\" failed: " + closed.failure().message);
        }
    }

    void call_dispatcher(Session& session, Contract::DispatchKind kind, int64_t token, const std::string& text,
        int64_t offset, int64_t flags)
    {
        const auto& lua = *session.lua;
        lua.registry().get_function_ref(session.dispatcher_ref);
        lua.set_integer(static_cast<int64_t>(kind));
        lua.set_integer(token);
        lua.set_string(text);
        lua.set_integer(offset);
        lua.set_integer(flags);
        lua.call_function(5, 0);
    }

    void close_subscription(Session& session, Subscription& subscription)
    {
        subscription.open.store(false);
        std::scoped_lock lock(session.mutex);
        session.subscriptions.erase(subscription.id);
    }

    // Reads at most Contract::tail_read_bytes_per_pass in total, at most
    // tail_read_bytes_per_subscription from each file, going round-robin by
    // subscription id from where the previous pass stopped.
    void collect_tail_deliveries()
    {
        std::vector<std::pair<std::shared_ptr<Session>, std::shared_ptr<Subscription>>> all;
        {
            std::scoped_lock lock(sessions_mutex_);
            for (auto& [_, session] : sessions_)
            {
                if (!session->active.load()) continue;
                std::scoped_lock session_lock(session->mutex);
                for (auto& [__, subscription] : session->subscriptions) all.emplace_back(session, subscription);
            }
        }
        if (all.empty()) return;
        std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.second->id < b.second->id; });
        std::size_t start = 0;
        while (start < all.size() && all[start].second->id <= last_polled_subscription_) ++start;
        if (start == all.size()) start = 0;

        std::vector<Pending> batch;
        const auto now = std::chrono::steady_clock::now();
        auto remaining = static_cast<std::size_t>(Contract::tail_read_bytes_per_pass);
        for (std::size_t step = 0; step < all.size() && remaining > 0; ++step)
        {
            auto& [session, subscription] = all[(start + step) % all.size()];
            last_polled_subscription_ = subscription->id;
            if (!subscription->open.load()) continue;
            std::vector<Core::TailDelivery> out;
            std::size_t consumed = 0;
            const auto allowance =
                std::min(remaining, static_cast<std::size_t>(Contract::tail_read_bytes_per_subscription));
            auto polled = subscription->file->poll(out, allowance, now, consumed);
            remaining -= std::min(consumed, remaining);
            for (auto& delivery : out)
            {
                batch.push_back(Pending{session, subscription, Contract::DispatchKind::data, std::move(delivery.text),
                    delivery.offset, delivery.flags});
            }
            if (!polled)
            {
                const auto& failed = polled.failure();
                report(subscription->path + ": " + std::string(Contract::to_string(failed.code)) + " " + failed.message);
                close_subscription(*session, *subscription);
                batch.push_back(Pending{session, subscription, Contract::DispatchKind::closed,
                    std::string(Contract::to_string(failed.code)) + "\t" + failed.message, 0, 0});
            }
        }
        if (batch.empty()) return;
        (void)backlog_.release_buffer();
        backlog_.load(std::move(batch));
    }

    bool deliver(Pending& pending)
    {
        auto& session = *pending.session;
        if (!session.active.load()) return false;
        if (pending.kind == Contract::DispatchKind::data && !pending.subscription->open.load()) return false;
        try
        {
            call_dispatcher(session, pending.kind, pending.subscription->token, pending.text, pending.offset, pending.flags);
            delivered_.fetch_add(1, std::memory_order_relaxed);
        }
        catch (const std::exception& error)
        {
            report("Tail callback for " + pending.subscription->path + " failed; subscription closed: " + error.what());
            close_subscription(session, *pending.subscription);
        }
        catch (...)
        {
            report("Tail callback for " + pending.subscription->path + " failed; subscription closed: unknown exception");
            close_subscription(session, *pending.subscription);
        }
        return true;
    }
};

// --- Natives -----------------------------------------------------------------

using Body = Reply (*)(FileBridgeMod&, Call&);

struct NativeEntry
{
    std::string_view name;
    std::string_view shape; // i = integer, s = string
    Body body;
    bool needs_roots;
};

// Normalizes, resolves, probes and checks one path for a native call.
struct Target
{
    std::string lexical;
    std::string real;
    Win32::Probe probe;
};

struct Context
{
    FileBridgeMod& bridge;
    std::shared_ptr<Session> session;
    std::shared_ptr<const Policy> policy;
    Core::CheckEnvironment environment;
    Core::Checker checker;

    Context(FileBridgeMod& owner, std::shared_ptr<Session> s, std::shared_ptr<const Policy> p)
        : bridge(owner), session(std::move(s)), policy(std::move(p)), environment(owner.environment(*session)),
          checker(*policy, environment)
    {
    }

    Outcome<Target> target(std::string_view operation, const std::string& path, Access access, bool follow_leaf,
        bool bypasses_backup = false, std::optional<TargetKind> kind = std::nullopt)
    {
        auto lexical = Core::normalize(path, session->locations, &Win32::equal_fold);
        if (!lexical)
        {
            return Failure{lexical.failure().code, std::string(operation) + ": " + lexical.failure().message};
        }
        auto real = bridge.resolver().resolve(lexical.value(), follow_leaf);
        if (!real) return real.failure();
        auto probed = Win32::probe(real.value());
        if (!probed) return probed.failure();
        const auto chosen = kind ? *kind : probed.value().kind == Win32::Kind::directory ? TargetKind::directory : TargetKind::file;
        Core::CheckRequest request{operation, path, access, chosen, bypasses_backup};
        auto checked = checker.check_real(real.value(), request);
        if (!checked) return checked.failure();
        return Target{std::move(lexical.value()), std::move(real.value()), probed.value()};
    }

    std::string backup_for(const std::string& real)
    {
        const auto* saves = checker.location_real("savegames");
        return saves && Core::is_within(real, *saves, &Win32::equal_fold) ? Core::backup_sibling(real) : std::string{};
    }
};

// Reads (session, policy) and builds the call context in `slot` (in place:
// the Checker refers to the context's own environment, so it never moves).
std::optional<Reply> open_context(FileBridgeMod& bridge, Call& call, std::optional<Context>& slot)
{
    auto session = bridge.find_session(call.integer());
    const auto policy_id = call.integer();
    if (!session) return failure(ErrorCode::invalid, session_gone());
    auto policy = bridge.find_policy(*session, policy_id);
    if (!policy) return failure(ErrorCode::invalid, "unknown or released environment");
    slot.emplace(bridge, std::move(session), std::move(policy));
    return std::nullopt;
}

Status parent_exists(const std::string& operation, const std::string& real)
{
    const auto parent = Win32::probe(Core::parent_path(real));
    if (!parent) return parent.failure();
    if (parent.value().kind != Win32::Kind::directory)
    {
        return Failure{ErrorCode::not_found, operation + " " + real + ": the folder doesn't exist"};
    }
    return Done{};
}

Outcome<std::string> decode(Call& call)
{
    const auto length = call.integer();
    const auto escaped = call.string();
    std::string data;
    if (!Contract::decode_escaped(escaped, length, data)) return Failure{ErrorCode::invalid, "malformed content encoding"};
    return data;
}

#define FILE_BRIDGE_CONTEXT(name)                                        \
    std::optional<Context> name##_slot;                                  \
    if (auto refused = open_context(bridge, call, name##_slot)) return *refused; \
    auto& name = *name##_slot

Reply get_version(FileBridgeMod&, Call&)
{
    return success(std::string(UE4SSLFB_VERSION));
}

Reply get_capabilities(FileBridgeMod&, Call&)
{
    return success(Contract::api_version, true, true, true, true, Contract::max_read_bytes,
        std::string(Contract::target_ue4ss_commit));
}

Reply get_dispatch_stats(FileBridgeMod& bridge, Call&)
{
    return success(bridge.active_subscriptions(), bridge.delivered(), bridge.exhausted_passes());
}

Reply locations(FileBridgeMod& bridge, Call& call)
{
    auto session = bridge.find_session(call.integer());
    if (!session) return failure(ErrorCode::invalid, session_gone());
    std::vector<Core::LocationRecord> records;
    for (const auto& location : session->locations.entries)
    {
        if (!location.enabled) continue;
        bool exists = false;
        if (!location.absolute.empty())
        {
            const auto probed = Win32::probe(location.absolute);
            exists = probed && probed.value().kind == Win32::Kind::directory;
        }
        records.push_back(Core::LocationRecord{location.name, location.absolute, location.root, exists});
    }
    return success(Core::encode_locations(records));
}

Reply normalize(FileBridgeMod& bridge, Call& call)
{
    auto session = bridge.find_session(call.integer());
    const auto path = call.string();
    if (!session) return failure(ErrorCode::invalid, session_gone());
    auto result = Core::normalize(path, session->locations, &Win32::equal_fold);
    if (!result) return failure(result.failure().code, "Normalize: " + result.failure().message);
    return success(std::move(result.value()));
}

Reply safe_policy(FileBridgeMod& bridge, Call& call)
{
    auto session = bridge.find_session(call.integer());
    if (!session) return failure(ErrorCode::invalid, session_gone());
    return success(session->safe_policy);
}

Reply add_path(FileBridgeMod& bridge, Call& call)
{
    FILE_BRIDGE_CONTEXT(context);
    const auto path = call.string();
    const auto mode = call.string();
    const auto extensions = call.string();
    const auto flags = call.integer();
    auto lexical = Core::normalize(path, context.session->locations, &Win32::equal_fold);
    if (!lexical) return failure(lexical.failure().code, "AddPath: " + lexical.failure().message);
    auto real = bridge.resolver().resolve(lexical.value(), true);
    if (!real) return failure(real.failure());
    const auto* game = context.checker.location_real("game");
    const auto* user = context.checker.location_real("user");
    if (!((game && Core::is_within(real.value(), *game, &Win32::equal_fold)) ||
            (user && Core::is_within(real.value(), *user, &Win32::equal_fold))))
    {
        return failure(ErrorCode::outside_root, "AddPath " + path + ": resolves outside the game and user folders");
    }
    auto grant = Core::make_grant(path, lexical.value(), real.value(), mode, extensions, flags);
    if (!grant) return failure(grant.failure());
    auto next = Core::add_grant(*context.policy, std::move(grant.value()), &Win32::equal_fold);
    if (!next) return failure(next.failure());
    auto& session = *context.session;
    std::scoped_lock lock(session.mutex);
    if (static_cast<int64_t>(session.policies.size()) - 1 >= Contract::max_policies_per_session)
    {
        return failure(ErrorCode::io, "AddPath: a Lua environment holds at most 4096 environments");
    }
    const auto id = bridge.next_policy_id();
    session.policies.emplace(id, std::make_shared<const Policy>(std::move(next.value())));
    return success(id);
}

Reply release_policy(FileBridgeMod& bridge, Call& call)
{
    auto session = bridge.find_session(call.integer());
    const auto id = call.integer();
    if (!session) return failure(ErrorCode::invalid, session_gone());
    if (id != session->safe_policy)
    {
        std::scoped_lock lock(session->mutex);
        session->policies.erase(id);
    }
    return success(true);
}

Reply exists(FileBridgeMod& bridge, Call& call)
{
    FILE_BRIDGE_CONTEXT(context);
    auto target = context.target("Exists", call.string(), Access::stat, true);
    if (!target) return failure(target.failure());
    return success(target.value().probe.kind != Win32::Kind::missing);
}

Reply stat(FileBridgeMod& bridge, Call& call)
{
    FILE_BRIDGE_CONTEXT(context);
    const auto path = call.string();
    auto target = context.target("Stat", path, Access::stat, true);
    if (!target) return failure(target.failure());
    if (target.value().probe.kind == Win32::Kind::missing) return failure(ErrorCode::not_found, "Stat " + path + ": not found");
    auto info = Win32::stat(target.value().real);
    if (!info) return failure(info.failure());
    bool link = false;
    if (auto own = bridge.resolver().resolve(target.value().lexical, false))
    {
        const auto probed = Win32::probe(own.value());
        link = probed && probed.value().link;
    }
    const auto& value = info.value();
    return success(value.type, value.size, seconds(value.modified), seconds(value.created), value.read_only, link);
}

Reply list(FileBridgeMod& bridge, Call& call)
{
    FILE_BRIDGE_CONTEXT(context);
    const auto path = call.string();
    auto target = context.target("List", path, Access::stat, true);
    if (!target) return failure(target.failure());
    std::size_t undecodable = 0;
    auto entries = Win32::list(target.value().real, undecodable);
    if (!entries) return failure(entries.failure());
    std::size_t unsafe = 0;
    auto records = Core::encode_list(std::move(entries.value()), unsafe);
    if (undecodable + unsafe != 0)
    {
        report("List " + path + ": skipped " + std::to_string(undecodable + unsafe) +
            " name(s) that can't be returned (unpaired surrogates, tabs or newlines)");
    }
    return success(std::move(records));
}

Reply read_text(FileBridgeMod& bridge, Call& call)
{
    FILE_BRIDGE_CONTEXT(context);
    auto target = context.target("ReadText", call.string(), Access::read, true);
    if (!target) return failure(target.failure());
    auto content = Win32::read(target.value().real, 0, -1, Contract::max_read_bytes);
    if (!content) return failure(content.failure());
    return success(std::string(Core::strip_utf8_bom(content.value())));
}

Reply read_bytes(FileBridgeMod& bridge, Call& call)
{
    FILE_BRIDGE_CONTEXT(context);
    const auto path = call.string();
    const auto offset = call.integer();
    const auto length = call.integer();
    if (offset < 0 || length < -1) return failure(ErrorCode::invalid, "ReadBytes " + path + ": offset or length out of range");
    auto target = context.target("ReadBytes", path, Access::read, true);
    if (!target) return failure(target.failure());
    auto content = Win32::read(target.value().real, offset, length, Contract::max_read_bytes);
    if (!content) return failure(content.failure());
    return success(std::move(content.value()));
}

Reply make_dir(FileBridgeMod& bridge, Call& call)
{
    FILE_BRIDGE_CONTEXT(context);
    const auto path = call.string();
    auto target = context.target("MakeDir", path, Access::make_dir, true, false, TargetKind::directory);
    if (!target) return failure(target.failure());
    if (target.value().probe.kind == Win32::Kind::file) return failure(ErrorCode::exists, "MakeDir " + path + ": a file is in the way");
    // moddata and temp are created by the bridge (with .owner) once the target is granted.
    auto prepared = bridge.prepare_moddata(*context.session, context.checker, target.value().real);
    if (!prepared) return failure(prepared.failure());
    // Missing folders from the target upwards; each must be granted.
    std::vector<std::string> missing;
    std::string current = target.value().real;
    for (;;)
    {
        const auto probed = Win32::probe(current);
        if (!probed) return failure(probed.failure());
        if (probed.value().kind == Win32::Kind::directory) break;
        if (probed.value().kind == Win32::Kind::file) return failure(ErrorCode::exists, "MakeDir " + current + ": a file is in the way");
        if (Core::segment_count(current) <= 1) return failure(ErrorCode::not_found, "MakeDir " + path + ": the drive doesn't exist");
        missing.push_back(current);
        current = Core::parent_path(current);
    }
    for (const auto& folder : missing)
    {
        Core::CheckRequest request{"MakeDir", path, Access::make_dir, TargetKind::directory, false};
        auto checked = context.checker.check_real(folder, request);
        if (!checked) return failure(checked.failure());
    }
    for (auto it = missing.rbegin(); it != missing.rend(); ++it)
    {
        auto made = Win32::create_directory(*it);
        if (!made) return failure(made.failure());
    }
    return success(true);
}

Reply write_text(FileBridgeMod& bridge, Call& call)
{
    FILE_BRIDGE_CONTEXT(context);
    const auto path = call.string();
    auto data = decode(call);
    const auto flags = call.integer();
    if (!data) return failure(ErrorCode::invalid, "WriteText " + path + ": " + data.failure().message);
    if ((flags & ~Contract::WriteFlag::known) != 0) return failure(ErrorCode::invalid, "WriteText " + path + ": unknown flags");
    const bool atomic = (flags & Contract::WriteFlag::non_atomic) == 0;
    auto target = context.target("WriteText", path, Access::write, true, !atomic);
    if (!target) return failure(target.failure());
    const auto& real = target.value().real;
    if (target.value().probe.kind == Win32::Kind::directory) return failure(ErrorCode::invalid, "WriteText " + path + ": is a folder");
    if (target.value().probe.read_only) return failure(ErrorCode::read_only, "WriteText " + path + ": the file is read-only");
    auto prepared = bridge.prepare_moddata(*context.session, context.checker, real);
    if (!prepared) return failure(prepared.failure());
    auto parent = parent_exists("WriteText", real);
    if (!parent) return failure(parent.failure());
    auto written = atomic ? Win32::write_atomic(real, data.value(), context.backup_for(real))
                          : Win32::write_in_place(real, data.value());
    if (!written) return failure(written.failure());
    return success(true);
}

Reply append(FileBridgeMod& bridge, Call& call)
{
    FILE_BRIDGE_CONTEXT(context);
    const auto path = call.string();
    auto data = decode(call);
    if (!data) return failure(ErrorCode::invalid, "Append " + path + ": " + data.failure().message);
    auto target = context.target("Append", path, Access::append, true);
    if (!target) return failure(target.failure());
    const auto& real = target.value().real;
    if (target.value().probe.kind == Win32::Kind::directory) return failure(ErrorCode::invalid, "Append " + path + ": is a folder");
    auto prepared = bridge.prepare_moddata(*context.session, context.checker, real);
    if (!prepared) return failure(prepared.failure());
    auto parent = parent_exists("Append", real);
    if (!parent) return failure(parent.failure());
    auto written = Win32::append(real, data.value());
    if (!written) return failure(written.failure());
    return success(true);
}

Reply remove(FileBridgeMod& bridge, Call& call)
{
    FILE_BRIDGE_CONTEXT(context);
    const auto path = call.string();
    auto target = context.target("Remove", path, Access::delete_entry, false);
    if (!target) return failure(target.failure());
    if (target.value().probe.kind == Win32::Kind::missing) return failure(ErrorCode::not_found, "Remove " + path + ": not found");
    auto removed = Win32::remove_entry(target.value().real);
    if (!removed) return failure(removed.failure());
    return success(true);
}

Reply remove_tree(FileBridgeMod& bridge, Call& call)
{
    FILE_BRIDGE_CONTEXT(context);
    const auto path = call.string();
    auto target = context.target("RemoveTree", path, Access::delete_entry, false, false, TargetKind::directory);
    if (!target) return failure(target.failure());
    auto entries = Win32::enumerate_tree(target.value().real);
    if (!entries) return failure(entries.failure());
    for (const auto& entry : entries.value())
    {
        Core::CheckRequest request{"RemoveTree", entry.path, Access::delete_entry,
            entry.directory ? TargetKind::directory : TargetKind::file, false};
        auto checked = context.checker.check_real(entry.path, request);
        if (!checked) return failure(checked.failure());
    }
    auto removed = Win32::remove_tree_entries(entries.value());
    if (!removed) return failure(removed.failure());
    return success(true);
}

Reply copy(FileBridgeMod& bridge, Call& call)
{
    FILE_BRIDGE_CONTEXT(context);
    const auto from = call.string();
    const auto to = call.string();
    const auto flags = call.integer();
    if ((flags & ~Contract::CopyMoveFlag::known) != 0) return failure(ErrorCode::invalid, "Copy: unknown flags");
    const bool overwrite = (flags & Contract::CopyMoveFlag::overwrite) != 0;
    auto source = context.target("Copy", from, Access::read, true);
    if (!source) return failure(source.failure());
    auto destination = context.target("Copy", to, Access::write, true);
    if (!destination) return failure(destination.failure());
    const auto& real = destination.value().real;
    auto prepared = bridge.prepare_moddata(*context.session, context.checker, real);
    if (!prepared) return failure(prepared.failure());
    auto parent = parent_exists("Copy", real);
    if (!parent) return failure(parent.failure());
    auto copied = Win32::copy_file(source.value().real, real, overwrite, overwrite ? context.backup_for(real) : std::string{});
    if (!copied) return failure(copied.failure());
    return success(true);
}

Reply move(FileBridgeMod& bridge, Call& call)
{
    FILE_BRIDGE_CONTEXT(context);
    const auto from = call.string();
    const auto to = call.string();
    const auto flags = call.integer();
    if ((flags & ~Contract::CopyMoveFlag::known) != 0) return failure(ErrorCode::invalid, "Move: unknown flags");
    const bool overwrite = (flags & Contract::CopyMoveFlag::overwrite) != 0;
    auto source = context.target("Move", from, Access::move_source, false);
    if (!source) return failure(source.failure());
    auto destination = context.target("Move", to, Access::write, false);
    if (!destination) return failure(destination.failure());
    const auto& real = destination.value().real;
    auto prepared = bridge.prepare_moddata(*context.session, context.checker, real);
    if (!prepared) return failure(prepared.failure());
    auto parent = parent_exists("Move", real);
    if (!parent) return failure(parent.failure());
    auto moved = Win32::move_entry(source.value().real, real, overwrite, overwrite ? context.backup_for(real) : std::string{});
    if (!moved) return failure(moved.failure());
    return success(true);
}

Reply stream_open(FileBridgeMod& bridge, Call& call)
{
    FILE_BRIDGE_CONTEXT(context);
    const auto path = call.string();
    const auto flags = call.integer();
    if ((flags & ~Contract::OpenFlag::known) != 0) return failure(ErrorCode::invalid, "Open " + path + ": unknown flags");
    const bool truncate = (flags & Contract::OpenFlag::truncate) != 0;
    const bool create = (flags & Contract::OpenFlag::no_create) == 0;
    const bool manual = (flags & Contract::OpenFlag::manual_flush) != 0;
    // A truncating Open can't keep a .bak, so it bypasses the savegames backup (contract step 9).
    auto target = context.target("Open", path, truncate ? Access::write : Access::append, true, truncate);
    if (!target) return failure(target.failure());
    const auto& real = target.value().real;
    auto prepared = bridge.prepare_moddata(*context.session, context.checker, real);
    if (!prepared) return failure(prepared.failure());
    auto parent = parent_exists("Open", real);
    if (!parent) return failure(parent.failure());
    {
        std::scoped_lock lock(context.session->mutex);
        if (static_cast<int64_t>(context.session->streams.size()) >= Contract::max_streams_per_session)
        {
            return failure(ErrorCode::io, "Open " + path + ": a Lua environment holds at most 64 open streams");
        }
    }
    auto stream = Win32::Stream::open(real, truncate, create, manual);
    if (!stream) return failure(stream.failure());
    const auto id = bridge.next_stream_id();
    std::shared_ptr<Win32::Stream> opened(std::move(stream.value()));
    {
        // Re-checked at insert: another Lua thread of this mod may have opened one meanwhile.
        std::scoped_lock lock(context.session->mutex);
        if (static_cast<int64_t>(context.session->streams.size()) < Contract::max_streams_per_session)
        {
            context.session->streams.emplace(id, opened);
            opened.reset();
        }
    }
    if (opened)
    {
        (void)opened->close();
        return failure(ErrorCode::io, "Open " + path + ": a Lua environment holds at most 64 open streams");
    }
    return success(id, target.value().lexical);
}

std::shared_ptr<Win32::Stream> find_stream(Session& session, int64_t id)
{
    std::scoped_lock lock(session.mutex);
    const auto found = session.streams.find(id);
    return found == session.streams.end() ? nullptr : found->second;
}

Reply stream_write(FileBridgeMod& bridge, Call& call)
{
    auto session = bridge.find_session(call.integer());
    const auto id = call.integer();
    auto data = decode(call);
    if (!session) return failure(ErrorCode::invalid, session_gone());
    if (!data) return failure(ErrorCode::invalid, "Write: " + data.failure().message);
    auto stream = find_stream(*session, id);
    if (!stream) return failure(ErrorCode::invalid, "stream is closed");
    auto written = stream->write(data.value());
    if (!written) return failure(written.failure());
    return success(true);
}

Reply stream_flush(FileBridgeMod& bridge, Call& call)
{
    auto session = bridge.find_session(call.integer());
    const auto id = call.integer();
    if (!session) return failure(ErrorCode::invalid, session_gone());
    auto stream = find_stream(*session, id);
    if (!stream) return failure(ErrorCode::invalid, "stream is closed");
    auto flushed = stream->flush();
    if (!flushed) return failure(flushed.failure());
    return success(true);
}

Reply stream_close(FileBridgeMod& bridge, Call& call)
{
    auto session = bridge.find_session(call.integer());
    const auto id = call.integer();
    if (!session) return failure(ErrorCode::invalid, session_gone());
    std::shared_ptr<Win32::Stream> stream;
    {
        std::scoped_lock lock(session->mutex);
        const auto found = session->streams.find(id);
        if (found != session->streams.end())
        {
            stream = found->second;
            session->streams.erase(found);
        }
    }
    if (stream)
    {
        auto closed = stream->close();
        if (!closed) return failure(closed.failure());
    }
    return success(true);
}

Reply tail_open(FileBridgeMod& bridge, Call& call)
{
    FILE_BRIDGE_CONTEXT(context);
    const auto path = call.string();
    const auto token = call.integer();
    const auto flags = call.integer();
    if (token <= 0) return failure(ErrorCode::invalid, "Tail " + path + ": invalid token");
    if ((flags & ~Contract::TailFlag::known) != 0) return failure(ErrorCode::invalid, "Tail " + path + ": unknown flags");
    auto target = context.target("Tail", path, Access::read, true);
    if (!target) return failure(target.failure());
    {
        std::scoped_lock lock(context.session->mutex);
        if (static_cast<int64_t>(context.session->subscriptions.size()) >= Contract::max_subscriptions_per_session)
        {
            return failure(ErrorCode::io, "Tail " + path + ": a Lua environment holds at most 64 subscriptions");
        }
    }
    auto file = Win32::TailFile::open(target.value().real, (flags & Contract::TailFlag::chunks) != 0,
        (flags & Contract::TailFlag::from_start) != 0);
    if (!file) return failure(file.failure());
    auto subscription = std::make_shared<Subscription>();
    subscription->id = bridge.next_subscription_id();
    subscription->token = token;
    subscription->path = target.value().lexical;
    subscription->file = std::move(file.value());
    {
        // Re-checked at insert: another Lua thread of this mod may have subscribed meanwhile.
        std::scoped_lock lock(context.session->mutex);
        if (static_cast<int64_t>(context.session->subscriptions.size()) >= Contract::max_subscriptions_per_session)
        {
            return failure(ErrorCode::io, "Tail " + path + ": a Lua environment holds at most 64 subscriptions");
        }
        context.session->subscriptions.emplace(subscription->id, subscription);
    }
    return success(subscription->id, target.value().lexical);
}

Reply tail_close(FileBridgeMod& bridge, Call& call)
{
    auto session = bridge.find_session(call.integer());
    const auto id = call.integer();
    if (!session) return failure(ErrorCode::invalid, session_gone());
    std::scoped_lock lock(session->mutex);
    const auto found = session->subscriptions.find(id);
    if (found != session->subscriptions.end())
    {
        found->second->open.store(false);
        session->subscriptions.erase(found);
    }
    return success(true);
}

#undef FILE_BRIDGE_CONTEXT

// Same order as Contract::native_functions; checked at compile time below.
constexpr std::array<NativeEntry, 26> natives{{
    {"GetVersion", "", &get_version, false},
    {"GetCapabilities", "", &get_capabilities, false},
    {"GetDispatchStats", "", &get_dispatch_stats, false},
    {"Locations", "i", &locations, false},
    {"Normalize", "is", &normalize, true},
    {"SafePolicy", "i", &safe_policy, false},
    {"AddPath", "iisssi", &add_path, true},
    {"ReleasePolicy", "ii", &release_policy, false},
    {"Exists", "iis", &exists, true},
    {"Stat", "iis", &stat, true},
    {"List", "iis", &list, true},
    {"ReadText", "iis", &read_text, true},
    {"ReadBytes", "iisii", &read_bytes, true},
    {"MakeDir", "iis", &make_dir, true},
    {"WriteText", "iisisi", &write_text, true},
    {"Append", "iisis", &append, true},
    {"Remove", "iis", &remove, true},
    {"RemoveTree", "iis", &remove_tree, true},
    {"Copy", "iissi", &copy, true},
    {"Move", "iissi", &move, true},
    {"StreamOpen", "iisi", &stream_open, true},
    {"StreamWrite", "iiis", &stream_write, false},
    {"StreamFlush", "ii", &stream_flush, false},
    {"StreamClose", "ii", &stream_close, false},
    {"TailOpen", "iisii", &tail_open, true},
    {"TailClose", "ii", &tail_close, false},
}};

constexpr bool natives_match_contract()
{
    if (natives.size() != Contract::native_functions.size()) return false;
    for (std::size_t i = 0; i < natives.size(); ++i)
    {
        if (natives[i].name != Contract::native_functions[i].name) return false;
        if (static_cast<int32_t>(natives[i].shape.size()) != Contract::native_functions[i].arity) return false;
    }
    return true;
}
static_assert(natives_match_contract(), "native table differs from FileBridgeNativeContract.hpp");

template<std::size_t Index>
int native_entry(const Lua& lua)
{
    const auto& entry = natives[Index];
    Reply reply;
    try
    {
        if (!shape_matches(lua, entry.shape))
        {
            reply = failure(ErrorCode::invalid, std::string(entry.name) + ": bad arguments");
        }
        else if (!active_bridge)
        {
            reply = failure(ErrorCode::io, std::string(entry.name) + ": the bridge is not active");
        }
        else if (entry.needs_roots && !active_bridge->roots_ok())
        {
            reply = failure(ErrorCode::io, std::string(entry.name) + ": file access is unavailable: " + active_bridge->roots_error());
        }
        else
        {
            Call call(lua);
            reply = entry.body(*active_bridge, call);
        }
    }
    catch (const std::exception& error)
    {
        reply = failure(ErrorCode::io, std::string(entry.name) + ": internal error: " + error.what());
    }
    catch (...)
    {
        reply = failure(ErrorCode::io, std::string(entry.name) + ": internal error");
    }
    return push(lua, reply);
}

template<std::size_t... Index>
void register_all(Lua& lua, std::index_sequence<Index...>)
{
    (lua.register_function(std::string(Contract::native_prefix) + std::string(natives[Index].name),
         Lua::LuaFunction{&native_entry<Index>}),
        ...);
}

void FileBridgeMod::register_natives(Lua& lua)
{
    register_all(lua, std::make_index_sequence<natives.size()>{});
}
}

namespace
{
UE4SSLEB_ModHandle start_implementation()
{
    return new FileBridgeMod();
}

UE4SSLEB_UninstallResult uninstall_implementation(UE4SSLEB_ModHandle mod)
{
    delete static_cast<FileBridgeMod*>(mod);
    return UE4SSLEB_UNINSTALL_CAN_UNLOAD;
}

static_assert(UE4SSLEB_IMPLEMENTATION_MAGIC == Contract::implementation_magic,
    "UE4SSLEB_IMPLEMENTATION_MAGIC must be the file bridge's LFB1");
}

LEX_EXPORT_IMPLEMENTATION(UE4SSLFB_VERSION_MAJOR, UE4SSLFB_VERSION_MINOR, UE4SSLFB_VERSION_PATCH,
    &start_implementation, &uninstall_implementation)
