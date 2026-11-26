#include "poll_store.hpp"
#include "chat_session.hpp"
#include <variant>

namespace
{
std::expected<chat::poll_summary, sqlite_error> read_poll_totals(sqlite_database& db, std::int64_t id, std::int64_t viewer)
{
    auto q = db.prepare(R"SQL(SELECT p.revision,p.closed,o.position,o.label,count(v.user),p.anonymous,p.multiple,p.correct,p.explanation,
        (SELECT count(*) FROM poll_votes WHERE message=p.message),
        coalesce((SELECT choices FROM poll_votes WHERE message=p.message AND user=?2),0)
        FROM polls p JOIN poll_options o ON o.message=p.message
        LEFT JOIN poll_votes v ON v.message=o.message AND (v.choices & (1 << (o.position-1)))<>0
        WHERE p.message=?1 GROUP BY o.position ORDER BY o.position)SQL",
                        id,
                        viewer);
    if (!q)
    {
        return std::unexpected(q.error());
    }
    chat::poll_summary state;
    for (;;)
    {
        auto row = q->step();
        if (!row)
        {
            return std::unexpected(row.error());
        }
        if (!*row)
        {
            break;
        }
        if (state.options.empty())
        {
            state.revision = q->column_int64(0);
            state.closed = q->column_int64(1) != 0;
            state.anonymous = q->column_int64(5) != 0;
            state.multiple = q->column_int64(6) != 0;
            state.quiz = q->column_int64(7) != 0;
            state.participants = q->column_int64(9);
            state.selection = q->column_int64(10);
            state.correct = q->column_int64(7);
            state.explanation = q->column_text(8);
        }
        state.options.push_back({q->column_int64(2), q->column_int64(4), std::string(q->column_text(3))});
    }
    if (!state.anonymous && state.participants)
    {
        auto recent = db.prepare(R"SQL(SELECT u.id,u.username,u.nickname,u.avatar_revision,a.user_id IS NOT NULL
            FROM poll_votes v JOIN users u ON u.id=v.user LEFT JOIN user_avatars a ON a.user_id=u.id
            WHERE v.message=? ORDER BY v.voted_revision DESC,u.id LIMIT 3)SQL",
                                 id);
        if (!recent)
        {
            return std::unexpected(recent.error());
        }
        for (;;)
        {
            auto row = recent->step();
            if (!row)
            {
                return std::unexpected(row.error());
            }
            if (!*row)
            {
                break;
            }
            state.recent.push_back({recent->column_int64(0),
                                    std::string(recent->column_text(1)),
                                    std::string(recent->column_text(2)),
                                    {recent->column_int64(3), recent->column_int64(4) != 0}});
        }
    }
    return state;
}

void project_ballot(chat::poll_summary& summary, std::int64_t selection)
{
    summary.selection = selection;
    if (!selection && !summary.closed)
    {
        summary.recent.clear();
        summary.correct = 0;
        summary.explanation.clear();
        for (auto& option : summary.options)
        {
            option.votes = 0;
        }
    }
}
struct[[= simdjson::deny_unknown_fields]] poll_params
{
    std::int64_t conversation = 0, message = 0;
    std::optional<std::int64_t> choice, after, choices;
};
struct poll_result
{
    chat::poll_state state;
    bool changed = false;
    std::vector<std::pair<std::int64_t, std::int64_t>> recipients;
};
enum class rejection
{
    unavailable,
    forbidden,
    closed,
    bad_choice
};
using outcome = std::variant<poll_result, chat::poll_voters, rejection>;

std::expected<outcome, sqlite_error> access_poll(sqlite_database& db, std::int64_t user, poll_params const& p, std::string_view method)
{
    bool const vote = method == "vote_poll", close = method == "close_poll", voters = method == "get_poll_voters";
    auto tx = vote || close ? sqlite_transaction::begin_immediate(db) : sqlite_transaction::begin_read(db);
    if (!tx)
    {
        return std::unexpected(tx.error());
    }
    auto access = db.prepare(R"SQL(SELECT m.body, m.sender_id=?1 OR c.owner_id=?1 OR own.is_admin,p.closed,
        coalesce((SELECT choices FROM poll_votes WHERE message=m.id AND user=?1),0),
        (SELECT count(*) FROM poll_options WHERE message=m.id),p.anonymous,p.multiple,p.correct,p.revision FROM readable_messages m
        JOIN polls p ON p.message=m.id JOIN conversations c ON c.id=m.conversation_id
        JOIN conversation_members own ON own.conversation_id=c.id AND own.user_id=?1
        WHERE c.id=?2 AND m.id=?3 AND NOT m.deleted AND m.id>own.visible_after)SQL",
                             user,
                             p.conversation,
                             p.message);
    if (!access)
    {
        return std::unexpected(access.error());
    }
    auto found = access->step();
    if (!found)
    {
        return std::unexpected(found.error());
    }
    if (!*found)
    {
        return rejection::unavailable;
    }
    poll_result result;
    result.state.question = access->column_text(0);
    result.state.can_close = access->column_int64(1) != 0;
    bool const closed = access->column_int64(2) != 0;
    auto const previous = access->column_int64(3);
    auto const count = access->column_int64(4);
    auto const choices = p.choices.value_or(0);
    bool const quiz = access->column_int64(7) != 0;
    if (voters)
    {
        if (access->column_int64(5) || (!closed && !previous))
        {
            return rejection::forbidden;
        }
        if (*p.choice > count)
        {
            return rejection::bad_choice;
        }
        int limit = p.after.value_or(0) ? 50 : 15;
        auto list = db.prepare(
            R"SQL(SELECT u.id,u.username,u.nickname,u.avatar_revision,a.user_id IS NOT NULL FROM poll_votes v JOIN users u ON u.id=v.user LEFT JOIN user_avatars a ON a.user_id=u.id
            WHERE v.message=? AND (v.choices & ?)<>0 AND v.user>? ORDER BY v.user LIMIT ?)SQL",
            p.message,
            chat::poll_bit(*p.choice),
            p.after.value_or(0),
            limit + 1);
        if (!list)
        {
            return std::unexpected(list.error());
        }
        chat::poll_voters page;
        for (;;)
        {
            auto row = list->step();
            if (!row)
            {
                return std::unexpected(row.error());
            }
            if (!*row)
            {
                return page;
            }
            if (std::ssize(page.members) == limit)
            {
                page.next = page.members.back().id;
                return page;
            }
            page.members.push_back({list->column_int64(0),
                                    std::string(list->column_text(1)),
                                    std::string(list->column_text(2)),
                                    {list->column_int64(3), list->column_int64(4) != 0}});
        }
    }
    if (close && !result.state.can_close)
    {
        return rejection::forbidden;
    }
    if (vote && closed)
    {
        return rejection::closed;
    }
    if (vote && (choices >= (1LL << count) || (!access->column_int64(6) && (choices & (choices - 1)))))
    {
        return rejection::bad_choice;
    }
    if (vote && previous != choices)
    {
        if (quiz && previous)
        {
            return rejection::forbidden;
        }
        auto err = choices ? db.exec(
                                 "INSERT INTO poll_votes(message,user,choices,voted_revision) VALUES(?,?,?,?) "
                                 "ON CONFLICT(message,user) DO UPDATE SET choices=excluded.choices,voted_revision=excluded.voted_revision",
                                 p.message,
                                 user,
                                 choices,
                                 access->column_int64(8) + 1)
                           : db.exec("DELETE FROM poll_votes WHERE message=? AND user=?", p.message, user);
        if (err)
        {
            return std::unexpected(err);
        }
        result.changed = true;
    }
    if (close && !closed)
    {
        result.changed = true;
    }
    if (result.changed)
    {
        if (auto err = db.exec("UPDATE polls SET revision=revision+1,closed=? WHERE message=?", int(close), p.message); err)
        {
            return std::unexpected(err);
        }
    }
    auto summary = read_poll_totals(db, p.message, user);
    if (!summary)
    {
        return std::unexpected(summary.error());
    }
    result.state.summary = std::move(*summary);
    if (result.changed)
    {
        // Read visibility and personal choices in the same revision as the shared totals.
        auto members = db.prepare(R"SQL(SELECT own.user_id, coalesce(v.choices,0)
            FROM conversation_members own LEFT JOIN poll_votes v ON v.message=?2 AND v.user=own.user_id
            WHERE own.conversation_id=?1 AND own.visible_after<?2)SQL",
                                  p.conversation,
                                  p.message);
        if (!members)
        {
            return std::unexpected(members.error());
        }
        for (;;)
        {
            auto row = members->step();
            if (!row)
            {
                return std::unexpected(row.error());
            }
            if (!*row)
            {
                break;
            }
            result.recipients.emplace_back(members->column_int64(0), members->column_int64(1));
        }
    }
    if (auto err = tx->commit(); err)
    {
        return std::unexpected(err);
    }
    return result;
}
}    // namespace

std::expected<chat::poll_summary, sqlite_error> read_poll_summary(sqlite_database& db, std::int64_t id, std::int64_t viewer)
{
    auto summary = read_poll_totals(db, id, viewer);
    if (summary)
    {
        project_ballot(*summary, summary->selection);
    }
    return summary;
}

boost::capy::task<simdjson::error_code> chat_session::handle_poll(json_rpc_request& request, std::string& response)
{
    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }
    if (!user_id_)
    {
        co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response);
    }
    auto invalid = [&] { return serialize_json_rpc_invalid_params(std::move(request.id), response); };
    bool const voters = request.method == "get_poll_voters";
    poll_params p;
    if (!request.params.read(p) || p.conversation <= 0 || p.message <= 0 || p.choice.has_value() != voters ||
        p.choices.has_value() != (request.method == "vote_poll") || p.choices.value_or(0) < 0 || p.after.value_or(0) < 0 || (p.after && !voters) ||
        p.choice.value_or(0) < 0 || (voters && *p.choice == 0))
    {
        co_return invalid();
    }
    auto result = co_await run_db([&, user = *user_id_](sqlite_database& db) { return access_poll(db, user, p, request.method); });
    if (!result)
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (auto* r = std::get_if<rejection>(&*result))
    {
        if (*r == rejection::bad_choice)
        {
            co_return invalid();
        }
        if (*r == rejection::unavailable)
        {
            co_return serialize_json_rpc_error(-32007, "Poll unavailable", std::move(request.id), response);
        }
        co_return serialize_json_rpc_error(
            -32009, *r == rejection::closed ? "Poll closed" : "Group permission denied", std::move(request.id), response);
    }
    if (auto* page = std::get_if<chat::poll_voters>(&*result))
    {
        co_return serialize_json_rpc_value(*page, std::move(request.id), response);
    }
    auto& poll = std::get<poll_result>(*result);
    if (poll.changed)
    {
        struct event
        {
            std::string_view jsonrpc, method;
            chat::poll_update params;
        };
        for (auto [user, choice] : poll.recipients)
        {
            auto sessions = users_.sessions(user);
            if (sessions.empty())
            {
                continue;
            }
            event notification{"2.0", "poll_updated", {p.conversation, p.message, poll.state.summary}};
            project_ballot(notification.params.summary, choice);
            std::string json;
            if (auto err = simdjson::builder::to_json_string(notification).get(json))
            {
                co_return err;
            }
            for (auto* session : sessions)
            {
                if (session != this)
                {
                    session->enqueue_message(json);
                }
            }
        }
    }
    project_ballot(poll.state.summary, poll.state.summary.selection);
    co_return serialize_json_rpc_value(poll.state, std::move(request.id), response);
}
