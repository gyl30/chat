#include "conversation_store.hpp"

#include <utility>

#include "timestamp.hpp"

namespace
{
std::expected<bool, sqlite_error> all_contacts(sqlite_database& db, std::int64_t actor, std::span<std::int64_t const> people)
{
    auto statement = db.prepare("SELECT 1 FROM contacts WHERE owner_id=?1 AND contact_id=?2");
    if (!statement)
    {
        return std::unexpected(std::move(statement.error()));
    }
    if (auto error = statement->bind(1, actor); error)
    {
        return std::unexpected(std::move(error));
    }
    for (auto person : people)
    {
        if (auto error = statement->bind(2, person); error)
        {
            return std::unexpected(std::move(error));
        }
        auto row = statement->step();
        if (!row || !*row)
        {
            return row;
        }
        if (auto error = statement->reset(); error)
        {
            return std::unexpected(std::move(error));
        }
    }
    return true;
}

constexpr std::string_view kInsertMember = R"SQL(
    INSERT INTO conversation_members(conversation_id,user_id,joined_message_id) VALUES(?1,?2,?3)
    ON CONFLICT(conversation_id,user_id) DO NOTHING
)SQL";
}    // namespace

std::expected<std::optional<std::int64_t>, sqlite_error> open_direct(sqlite_database& db, std::int64_t user, std::int64_t peer)
{
    auto transaction = sqlite_transaction::begin_immediate(db);
    if (!transaction)
    {
        return std::unexpected(std::move(transaction.error()));
    }
    auto allowed = all_contacts(db, user, std::span(&peer, 1));
    if (!allowed)
    {
        return std::unexpected(std::move(allowed.error()));
    }
    if (!*allowed)
    {
        return std::nullopt;
    }
    if (auto error = db.exec(
            R"SQL(
        INSERT INTO conversations(kind,direct_user_low,direct_user_high,activity)
        VALUES('direct',min(?1,?2),max(?1,?2),?3)
        ON CONFLICT(direct_user_low,direct_user_high) WHERE kind='direct' DO NOTHING
    )SQL",
            user,
            peer,
            epoch_milliseconds());
        error)
    {
        return std::unexpected(std::move(error));
    }
    auto found = db.prepare(R"SQL(
        SELECT id FROM conversations
        WHERE kind='direct' AND direct_user_low=min(?1,?2) AND direct_user_high=max(?1,?2)
    )SQL",
                            user,
                            peer);
    if (!found)
    {
        return std::unexpected(std::move(found.error()));
    }
    auto row = found->step();
    if (!row)
    {
        return std::unexpected(std::move(row.error()));
    }
    if (!*row)
    {
        return std::nullopt;
    }
    auto const id = found->column_int64(0);
    for (auto member : {user, peer})
    {
        if (auto error = db.exec(kInsertMember, id, member, 0); error)
        {
            return std::unexpected(std::move(error));
        }
    }
    if (auto error = transaction->commit(); error)
    {
        return std::unexpected(std::move(error));
    }
    return id;
}

std::expected<std::optional<std::int64_t>, sqlite_error> create_group(sqlite_database& db,
                                                                      std::int64_t owner,
                                                                      std::string_view title,
                                                                      std::span<std::int64_t const> members)
{
    auto transaction = sqlite_transaction::begin_immediate(db);
    if (!transaction)
    {
        return std::unexpected(std::move(transaction.error()));
    }
    auto allowed = all_contacts(db, owner, members);
    if (!allowed)
    {
        return std::unexpected(std::move(allowed.error()));
    }
    if (!*allowed)
    {
        return std::nullopt;
    }
    if (auto error = db.exec("INSERT INTO conversations(kind,title,owner_id,activity) VALUES('group',?1,?2,?3)", title, owner, epoch_milliseconds());
        error)
    {
        return std::unexpected(std::move(error));
    }
    auto const id = db.last_insert_rowid();
    if (auto error = db.exec(kInsertMember, id, owner, 0); error)
    {
        return std::unexpected(std::move(error));
    }
    for (auto member : members)
    {
        if (auto error = db.exec(kInsertMember, id, member, 0); error)
        {
            return std::unexpected(std::move(error));
        }
    }
    if (auto error = transaction->commit(); error)
    {
        return std::unexpected(std::move(error));
    }
    return id;
}

std::expected<std::vector<chat::conversation_member>, sqlite_error> read_conversation_members(sqlite_database& db,
                                                                                              std::int64_t conversation,
                                                                                              std::int64_t actor)
{
    auto members = db.prepare(R"SQL(
        SELECT u.id,u.username,u.id=c.owner_id,m.is_admin,u.avatar_revision,a.user_id IS NOT NULL,coalesce(p.until,0),u.nickname,coalesce(ct.remark,'')
        FROM conversation_members own
        JOIN conversations c ON c.id=own.conversation_id
        JOIN conversation_members m ON m.conversation_id=c.id
        JOIN users u ON u.id=m.user_id
        LEFT JOIN contacts ct ON ct.owner_id=?2 AND ct.contact_id=u.id
        LEFT JOIN user_avatars a ON a.user_id=u.id
        LEFT JOIN group_member_mutes p ON p.conversation_id=c.id AND p.user_id=u.id
        WHERE own.conversation_id=?1 AND own.user_id=?2 ORDER BY u.id
    )SQL",
                              conversation,
                              actor);
    if (!members)
    {
        return std::unexpected(std::move(members.error()));
    }
    std::vector<chat::conversation_member> result;
    for (;;)
    {
        auto row = members->step();
        if (!row)
        {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row)
        {
            return result;
        }
        auto role = chat::member_role::member;
        if (members->column_int64(2))
        {
            role = chat::member_role::owner;
        }
        else if (members->column_int64(3))
        {
            role = chat::member_role::admin;
        }
        result.push_back({members->column_int64(0),
                          std::string(members->column_text(1)),
                          role,
                          {members->column_int64(4), members->column_int64(5) != 0},
                          members->column_int64(6),
                          std::string(members->column_text(7)),
                          std::string(members->column_text(8))});
    }
}

std::expected<send_access, sqlite_error> can_send_to_conversation(sqlite_database& db, std::int64_t conversation, std::int64_t actor)
{
    auto access = db.prepare(R"SQL(
        SELECT coalesce(c.kind='group' AND own.is_admin=0 AND own.user_id<>c.owner_id
                        AND (g.conversation_id IS NOT NULL OR p.until=-1 OR p.until>?3),0)
        FROM conversations c JOIN conversation_members own ON c.id=own.conversation_id AND own.user_id=?1
        LEFT JOIN group_mutes g ON g.conversation_id=c.id
        LEFT JOIN group_member_mutes p ON p.conversation_id=c.id AND p.user_id=?1
        WHERE c.id=?2 AND (c.kind='group' OR EXISTS(
            SELECT 1 FROM contacts WHERE owner_id=?1
                AND contact_id=CASE WHEN c.direct_user_low=?1 THEN c.direct_user_high ELSE c.direct_user_low END))
    )SQL",
                             actor,
                             conversation,
                             epoch_milliseconds());
    if (!access)
    {
        return std::unexpected(std::move(access.error()));
    }
    auto row = access->step();
    if (!row)
    {
        return std::unexpected(row.error());
    }
    return !*row ? send_access::unavailable : access->column_int64(0) ? send_access::muted : send_access::allowed;
}

std::expected<std::optional<chat::member_role>, sqlite_error> read_group_role(sqlite_database& db, std::int64_t group, std::int64_t actor)
{
    auto statement = db.prepare(R"SQL(
        SELECT c.owner_id=m.user_id,m.is_admin
        FROM conversations c JOIN conversation_members m ON m.conversation_id=c.id
        WHERE c.kind='group' AND c.id=?1 AND m.user_id=?2
    )SQL",
                                group,
                                actor);
    if (!statement)
    {
        return std::unexpected(std::move(statement.error()));
    }
    auto row = statement->step();
    if (!row)
    {
        return std::unexpected(std::move(row.error()));
    }
    if (!*row)
    {
        return std::nullopt;
    }
    if (statement->column_int64(0))
    {
        return chat::member_role::owner;
    }
    return statement->column_int64(1) ? chat::member_role::admin : chat::member_role::member;
}

std::expected<std::variant<bool, group_member_rejection>, sqlite_error> set_group_text(
    sqlite_database& db, std::int64_t group, std::int64_t actor, group_text_field field, std::string_view text)
{
    auto transaction = sqlite_transaction::begin_immediate(db);
    if (!transaction)
    {
        return std::unexpected(std::move(transaction.error()));
    }
    auto role = read_group_role(db, group, actor);
    if (!role)
    {
        return std::unexpected(std::move(role.error()));
    }
    if (!*role)
    {
        return group_member_rejection::unavailable;
    }
    if (**role == chat::member_role::member)
    {
        return group_member_rejection::permission_denied;
    }
    auto const query = field == group_text_field::title         ? "UPDATE conversations SET title=?2 WHERE id=?1 AND title<>?2"
                       : field == group_text_field::description ? "UPDATE conversations SET description=?2 WHERE id=?1 AND description<>?2"
                                                                : "UPDATE conversations SET announcement=?2 WHERE id=?1 AND announcement<>?2";
    if (auto error = db.exec(query, group, text); error)
    {
        return std::unexpected(std::move(error));
    }
    bool const changed = db.changes() != 0;
    if (auto error = transaction->commit(); error)
    {
        return std::unexpected(std::move(error));
    }
    return changed;
}

std::expected<std::variant<bool, group_member_rejection>, sqlite_error> set_group_pin(sqlite_database& db,
                                                                                      std::int64_t group,
                                                                                      std::int64_t actor,
                                                                                      std::optional<std::int64_t> message)
{
    auto transaction = sqlite_transaction::begin_immediate(db);
    if (!transaction)
    {
        return std::unexpected(std::move(transaction.error()));
    }
    auto role = read_group_role(db, group, actor);
    if (!role)
    {
        return std::unexpected(std::move(role.error()));
    }
    if (!*role)
    {
        return group_member_rejection::unavailable;
    }
    if (**role == chat::member_role::member)
    {
        return group_member_rejection::permission_denied;
    }
    if (message)
    {
        auto target = db.prepare(R"SQL(
            SELECT 1 FROM messages m JOIN conversation_members own ON own.conversation_id=m.conversation_id
            WHERE m.conversation_id=?1 AND m.id=?2 AND m.deleted=0 AND own.user_id=?3 AND m.id>own.visible_after
        )SQL",
                                 group,
                                 *message,
                                 actor);
        if (!target)
        {
            return std::unexpected(std::move(target.error()));
        }
        auto row = target->step();
        if (!row)
        {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row)
        {
            return group_member_rejection::invalid_target;
        }
    }
    if (auto error = db.exec("UPDATE conversations SET pinned_message_id=?2 WHERE id=?1 AND pinned_message_id IS NOT ?2", group, message); error)
    {
        return std::unexpected(std::move(error));
    }
    bool const changed = db.changes() != 0;
    if (auto error = transaction->commit(); error)
    {
        return std::unexpected(std::move(error));
    }
    return changed;
}

std::expected<std::variant<bool, group_member_rejection>, sqlite_error> invite_group_members(sqlite_database& db,
                                                                                             std::int64_t group,
                                                                                             std::int64_t actor,
                                                                                             std::span<std::int64_t const> invitees)
{
    auto transaction = sqlite_transaction::begin_immediate(db);
    if (!transaction)
    {
        return std::unexpected(std::move(transaction.error()));
    }

    std::int64_t watermark = 0;
    {
        auto group_state = db.prepare(R"SQL(
            SELECT c.owner_id=?2 OR own.is_admin,
                   coalesce((SELECT max(id) FROM messages WHERE conversation_id=?1),0)
            FROM conversations c
            JOIN conversation_members own ON own.conversation_id=c.id AND own.user_id=?2
            WHERE c.id=?1 AND c.kind='group'
        )SQL",
                                      group,
                                      actor);
        if (!group_state)
        {
            return std::unexpected(std::move(group_state.error()));
        }
        auto row = group_state->step();
        if (!row)
        {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row)
        {
            return group_member_rejection::unavailable;
        }
        if (group_state->column_int64(0) == 0)
        {
            return group_member_rejection::permission_denied;
        }
        watermark = group_state->column_int64(1);
    }
    auto allowed = all_contacts(db, actor, invitees);
    if (!allowed)
    {
        return std::unexpected(std::move(allowed.error()));
    }
    if (!*allowed)
    {
        return group_member_rejection::contact_required;
    }

    bool added = false;
    for (auto invitee : invitees)
    {
        if (auto error = db.exec(kInsertMember, group, invitee, watermark); error)
        {
            return std::unexpected(std::move(error));
        }
        added = added || db.changes() != 0;
        if (auto error = db.exec("DELETE FROM group_join_requests WHERE conversation_id=?1 AND user_id=?2", group, invitee); error)
        {
            return std::unexpected(std::move(error));
        }
    }
    if (auto error = transaction->commit(); error)
    {
        return std::unexpected(std::move(error));
    }
    return added;
}

std::expected<std::variant<bool, group_member_rejection>, sqlite_error> change_group_member(
    sqlite_database& db, std::int64_t group, std::int64_t actor, group_member_action action, std::int64_t target)
{
    using enum group_member_action;
    using enum group_member_rejection;

    auto transaction = sqlite_transaction::begin_immediate(db);
    if (!transaction)
    {
        return std::unexpected(std::move(transaction.error()));
    }

    std::int64_t owner = 0;
    bool actor_admin = false;
    bool target_admin = false;
    bool target_member = false;
    {
        auto roles = db.prepare(R"SQL(
            SELECT c.owner_id,own.is_admin,other.is_admin
            FROM conversations c
            JOIN conversation_members own ON own.conversation_id=c.id AND own.user_id=?2
            LEFT JOIN conversation_members other ON other.conversation_id=c.id AND other.user_id=?3
            WHERE c.id=?1 AND c.kind='group'
        )SQL",
                                group,
                                actor,
                                target);
        if (!roles)
        {
            return std::unexpected(std::move(roles.error()));
        }
        auto row = roles->step();
        if (!row)
        {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row)
        {
            return unavailable;
        }
        owner = roles->column_int64(0);
        actor_admin = roles->column_int64(1) != 0;
        target_member = !roles->column_is_null(2);
        target_admin = roles->column_int64(2) != 0;
    }

    bool const owner_action = action == promote || action == demote || action == transfer;
    if ((owner_action && actor != owner) || (action == remove && actor != owner && !actor_admin) || (action == leave && actor == owner))
    {
        return permission_denied;
    }
    if (action != leave)
    {
        if (target == actor)
        {
            return invalid_target;
        }
        if (!target_member)
        {
            return member_not_found;
        }
    }

    switch (action)
    {
        case promote:
        case demote:
        {
            bool const enabled = action == promote;
            if (enabled == target_admin)
            {
                return false;
            }
            if (enabled)
            {
                auto current = db.prepare("SELECT count(*) FROM conversation_members WHERE conversation_id=?1 AND is_admin=1", group);
                if (!current)
                {
                    return std::unexpected(std::move(current.error()));
                }
                auto row = current->step();
                if (!row)
                {
                    return std::unexpected(std::move(row.error()));
                }
                if (*row && current->column_int64(0) >= 3)
                {
                    return admin_limit;
                }
            }
            if (auto error = db.exec(
                    "UPDATE conversation_members SET is_admin=?3 WHERE conversation_id=?1 AND user_id=?2", group, target, static_cast<int>(enabled));
                error)
            {
                return std::unexpected(std::move(error));
            }
            break;
        }
        case transfer:
            if (!target_admin)
            {
                return permission_denied;
            }
            if (auto error = db.exec(
                    R"SQL(
                UPDATE conversation_members SET is_admin=(user_id=?2)
                WHERE conversation_id=?1 AND user_id IN(?2,?3)
            )SQL",
                    group,
                    actor,
                    target);
                error)
            {
                return std::unexpected(std::move(error));
            }
            if (auto error = db.exec("UPDATE conversations SET owner_id=?2 WHERE id=?1", group, target); error)
            {
                return std::unexpected(std::move(error));
            }
            break;
        case remove:
            if (target == owner || (actor != owner && target_admin))
            {
                return permission_denied;
            }
            [[fallthrough]];
        case leave:
            if (auto error =
                    db.exec("DELETE FROM conversation_members WHERE conversation_id=?1 AND user_id=?2", group, action == leave ? actor : target);
                error)
            {
                return std::unexpected(std::move(error));
            }
            break;
    }

    if (auto error = transaction->commit(); error)
    {
        return std::unexpected(std::move(error));
    }
    return true;
}

std::expected<std::variant<bool, group_member_rejection>, sqlite_error> set_group_speech(
    sqlite_database& db, std::int64_t group, std::int64_t actor, std::int64_t user, std::int64_t until, bool mute_all)
{
    auto tx = sqlite_transaction::begin_immediate(db);
    if (!tx)
    {
        return std::unexpected(tx.error());
    }
    auto role = read_group_role(db, group, actor);
    if (!role)
    {
        return std::unexpected(role.error());
    }
    if (!*role || **role == chat::member_role::member)
    {
        return *role ? group_member_rejection::permission_denied : group_member_rejection::unavailable;
    }
    sqlite_error error;
    if (user)
    {
        auto target = read_group_role(db, group, user);
        if (!target)
        {
            return std::unexpected(target.error());
        }
        if (!*target || **target != chat::member_role::member)
        {
            return *target ? group_member_rejection::permission_denied : group_member_rejection::member_not_found;
        }
        error = until ? db.exec(R"SQL(INSERT INTO group_member_mutes(conversation_id,user_id,until) VALUES(?1,?2,?3)
                             ON CONFLICT(conversation_id,user_id) DO UPDATE SET until=excluded.until WHERE until<>excluded.until)SQL",
                                group,
                                user,
                                until)
                      : db.exec("DELETE FROM group_member_mutes WHERE conversation_id=? AND user_id=?", group, user);
    }
    else
    {
        error = mute_all ? db.exec("INSERT INTO group_mutes(conversation_id) VALUES(?) ON CONFLICT DO NOTHING", group)
                         : db.exec("DELETE FROM group_mutes WHERE conversation_id=?", group);
    }
    if (error)
    {
        return std::unexpected(error);
    }
    bool const changed = db.changes() != 0;
    if (auto e = tx->commit(); e)
    {
        return std::unexpected(e);
    }
    return changed;
}

std::expected<std::variant<std::vector<std::int64_t>, group_member_rejection>, sqlite_error> dissolve_group(sqlite_database& db,
                                                                                                            std::int64_t group,
                                                                                                            std::int64_t actor)
{
    auto tx = sqlite_transaction::begin_immediate(db);
    if (!tx)
    {
        return std::unexpected(tx.error());
    }
    auto role = read_group_role(db, group, actor);
    if (!role)
    {
        return std::unexpected(role.error());
    }
    if (!*role)
    {
        auto prior = db.prepare("SELECT 1 FROM dissolved_groups WHERE conversation_id=? AND owner_id=?", group, actor);
        if (!prior)
        {
            return std::unexpected(prior.error());
        }
        auto row = prior->step();
        if (!row)
        {
            return std::unexpected(row.error());
        }
        if (*row)
        {
            return std::vector<std::int64_t>{};
        }
        return group_member_rejection::unavailable;
    }
    if (**role != chat::member_role::owner)
    {
        return group_member_rejection::permission_denied;
    }
    std::vector<std::int64_t> recipients;
    {
        auto members = db.prepare("SELECT user_id FROM conversation_members WHERE conversation_id=?", group);
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
            recipients.push_back(members->column_int64(0));
        }
    }
    // Keep upload keys and multipart IDs until Abort succeeds. The now-empty shell
    // only exists for their foreign key. Existing attachment triggers release references.
    if (auto e = db.exec("INSERT INTO dissolved_groups(conversation_id,owner_id) VALUES(?,?)", group, actor); e)
    {
        return std::unexpected(e);
    }
    if (auto e = db.exec(
            "UPDATE attachment_uploads SET state='discarding',touched=? WHERE conversation=? AND state<>'discarding'", epoch_milliseconds(), group);
        e)
    {
        return std::unexpected(e);
    }
    for (auto const* sql : {"UPDATE conversations SET owner_id=NULL,invite_token=NULL,pinned_message_id=NULL WHERE id=?",
                            "DELETE FROM conversation_members WHERE conversation_id=?",
                            "DELETE FROM group_join_requests WHERE conversation_id=?",
                            "DELETE FROM group_member_mutes WHERE conversation_id=?",
                            "DELETE FROM group_mutes WHERE conversation_id=?",
                            "DELETE FROM conversation_files WHERE conversation_id=?",
                            "DELETE FROM messages WHERE conversation_id=?",
                            "DELETE FROM conversations WHERE id=?1 AND NOT EXISTS(SELECT 1 FROM attachment_uploads WHERE conversation=?1)"})
    {
        if (auto e = db.exec(sql, group); e)
        {
            return std::unexpected(e);
        }
    }
    if (auto e = tx->commit(); e)
    {
        return std::unexpected(e);
    }
    return recipients;
}
