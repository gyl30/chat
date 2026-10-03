#include "app.hpp"
namespace chat::tui
{
void app::members()
{
    auto const* conversation = data.active_conversation();
    if (!conversation || conversation->kind != conversation_kind::group || !online()) { return; }
    auto const id = data.active; auto const view = view_;
    client_->get_members(id, callback([this, id, view](auto value) {
        if (id != data.active || view != view_) { return; }
        if (!value) { error(value.error()); return; }
        data.members = std::move(*value);
    }));
}
void app::requests(bool) { data.status = "群管理将在下一阶段接入"; }
void app::group_command(std::string const& name, std::string)
{
    if (name == "members") { navigate(page::members); members(); }
    else if (name == "group") { navigate(page::group); members(); }
    else { data.status = "群管理将在下一阶段接入"; }
}
void app::group_done(bool) { conversations(); members(); }
void app::toggle_pick() {}
void app::finish_pick() {}
}
