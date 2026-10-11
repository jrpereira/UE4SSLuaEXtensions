#include <SessionAliasIndex.hpp>

#include <array>
#include <TestSupport.hpp>
#include <cstddef>
#include <thread>
#include <vector>

using namespace UE4SSLuaEventBridge;

int main()
{
    struct State {};
    struct Session {};

    State parent, main_thread, async_thread, hook_thread, unrelated;
    Session first_session, second_session;
    SessionAliasIndex<State, Session> sessions;

    sessions.bind(nullptr, &first_session);
    sessions.bind(&parent, nullptr);
    CHECK(sessions.size() == 0);

    sessions.bind(&parent, &first_session);
    sessions.bind(&main_thread, &first_session);
    sessions.bind(&async_thread, &first_session);
    sessions.bind(&hook_thread, &first_session);
    sessions.bind(&unrelated, &second_session);

    CHECK(sessions.size() == 5);
    CHECK(sessions.find(&parent) == &first_session);
    CHECK(sessions.find(&main_thread) == &first_session);
    CHECK(sessions.find(&async_thread) == &first_session);
    CHECK(sessions.find(&hook_thread) == &first_session);
    CHECK(sessions.find(&unrelated) == &second_session);

    sessions.bind(&parent, &second_session);
    CHECK(sessions.size() == 5);
    CHECK(sessions.find(&parent) == &second_session);
    sessions.bind(&parent, &first_session);

    CHECK(sessions.unbind(&first_session) == 4);
    CHECK(sessions.find(&parent) == nullptr);
    CHECK(sessions.find(&main_thread) == nullptr);
    CHECK(sessions.find(&unrelated) == &second_session);
    CHECK(sessions.size() == 1);

    constexpr std::size_t thread_count = 8;
    constexpr std::size_t aliases_per_thread = 32;
    std::array<State, thread_count * aliases_per_thread> concurrent_states{};
    std::array<Session, thread_count> concurrent_sessions{};
    SessionAliasIndex<State, Session> concurrent_index;
    std::vector<std::thread> workers;

    for (std::size_t thread = 0; thread < thread_count; ++thread)
    {
        workers.emplace_back([&, thread] {
            for (std::size_t alias = 0; alias < aliases_per_thread; ++alias)
            {
                const auto index = thread * aliases_per_thread + alias;
                concurrent_index.bind(&concurrent_states[index], &concurrent_sessions[thread]);
                CHECK(concurrent_index.find(&concurrent_states[index]) == &concurrent_sessions[thread]);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    CHECK(concurrent_index.size() == concurrent_states.size());

    workers.clear();
    for (std::size_t thread = 0; thread < thread_count; ++thread)
    {
        workers.emplace_back([&, thread] {
            CHECK(concurrent_index.unbind(&concurrent_sessions[thread]) == aliases_per_thread);
        });
    }
    for (auto& worker : workers) worker.join();
    CHECK(concurrent_index.size() == 0);
    return LexTests::finish("SessionAliasIndexTests");
}
