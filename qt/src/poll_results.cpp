#include "chat_widget.hpp"
#include "client_bridge.hpp"
#include "message_card.hpp"
#include "message_model.hpp"
#include <QLabel>
#include <QListView>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QVBoxLayout>

namespace
{
constexpr int first_page = 15;
constexpr int collapsed_rows = 10;

struct voter_group
{
    QWidget* section;
    QLabel* title;
    QLabel* tally;
    QPushButton* collapse;
    QVBoxLayout* rows;
    QPushButton* more;
    qint64 votes = 0;
    int wanted = 0;
    int limit() const { return wanted ? wanted : (votes <= first_page ? first_page : collapsed_rows); }
    quint64 request = 0;
    qint64 after = 0;
    // During a revision refresh keep the visible rows until the replacement page range arrives.
    QList<chat::poll_voter> members{};
};

class results_page final : public QWidget
{
   public:
    results_page(client_bridge& client,
                 chat_widget& chat,
                 qint64 room,
                 qint64 message,
                 QString question,
                 chat::poll_summary const& initial,
                 std::function<void(QWidget*)> back)
        : QWidget(&chat), client_(client), chat_(chat), room_(room), message_(message), back_(std::move(back))
    {
        setObjectName("pollResults");
        setWindowTitle(QStringLiteral("投票结果"));
        auto* outer = new QVBoxLayout(this);
        auto* bar = new QHBoxLayout;
        auto* previous = new QPushButton(QStringLiteral("‹ 返回"));
        previous->setObjectName("pollBack");
        connect(previous, &QPushButton::clicked, this, [this] { back_(this); });
        auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
        escape->setContext(Qt::WidgetWithChildrenShortcut);
        connect(escape, &QShortcut::activated, this, [this] { back_(this); });
        bar->addWidget(previous);
        bar->addWidget(new QLabel(QStringLiteral("投票结果")));
        bar->addStretch();
        outer->addLayout(bar);
        scroll_ = new QScrollArea;
        scroll_->setWidgetResizable(true);
        scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll_->setFrameShape(QFrame::NoFrame);
        outer->addWidget(scroll_);
        auto* body = new QWidget;
        column_ = new QVBoxLayout(body);
        column_->setAlignment(Qt::AlignTop);
        auto* heading = new QLabel(question);
        heading->setTextFormat(Qt::PlainText);
        heading->setWordWrap(true);
        heading->setObjectName("pollQuestionHeading");
        auto title = heading->font();
        title.setBold(true);
        heading->setFont(title);
        column_->addWidget(heading);
        status_ = new QLabel;
        status_->setObjectName("pollStatus");
        column_->addWidget(status_);
        scroll_->setWidget(body);
        connect(&client,
                &client_bridge::poll_received,
                this,
                [this, initial_revision = initial.revision](quint64 req, qint64 room, qint64 id, chat::poll_state state, QString error)
                {
                    if (req != query_ || room != room_ || id != message_)
                    {
                        return;
                    }
                    if (!error.isEmpty())
                    {
                        if (revision_ == initial_revision)
                        {
                            status_->setText(error);
                        }
                        return;
                    }
                    render(state.summary);
                });
        connect(&client,
                &client_bridge::poll_changed,
                this,
                [this](chat::poll_update update)
                {
                    if (update.message == message_ && update.conversation == room_)
                    {
                        render(update.summary);
                    }
                });
        connect(&client,
                &client_bridge::profile_changed,
                this,
                [this](qint64 id)
                {
                    for (auto it = groups_.begin(); it != groups_.end(); ++it)
                    {
                        if (std::ranges::find(it->members, id, &chat::poll_voter::id) != it->members.end())
                        {
                            it->members.clear();
                            it->after = 0;
                            request(it.key());
                        }
                    }
                });
        connect(&client, &client_bridge::voters_received, this, &results_page::received);
        connect(&chat,
                &chat_widget::history_discarded,
                this,
                [this](qint64 room, qint64 through)
                {
                    if (room == room_ && through >= message_)
                    {
                        back_(this);
                    }
                });
        connect(&client,
                &client_bridge::conversation_changed,
                this,
                [this](qint64 room, bool removed)
                {
                    if (room == room_ && removed)
                    {
                        back_(this);
                    }
                });
        connect(&client,
                &client_bridge::message_updated,
                this,
                [this](qint64 room, message_data m, QString)
                {
                    if (room == room_ && m.id == message_ && m.deleted)
                    {
                        back_(this);
                    }
                });
        connect(&client, &client_bridge::disconnected, this, [this] { back_(this); });
        render(initial);
        query_ = client_.get_poll(room_, message_);
        setFocusProxy(previous);
    }

   private:
    void request(qint64 choice)
    {
        auto& group = groups_[choice];
        group.more->setProperty("retry", false);
        group.more->setEnabled(false);
        group.request = client_.get_poll_voters(room_, message_, choice, group.after);
    }

    void update_controls(voter_group& g)
    {
        int shown = std::min<int>(g.limit(), g.members.size());
        g.more->setVisible(g.votes > shown);
        g.more->setEnabled(!g.request);
        g.more->setText(QStringLiteral("再显示 %1 位成员").arg(g.votes - shown));
        g.collapse->setVisible(g.wanted > 0 && g.votes > first_page);
        g.tally->setVisible(g.collapse->isHidden());
    }

    void populate(voter_group& g, qint64 choice)
    {
        // Reuse member buttons by identity; unchanged rows retain focus and their avatar subscriptions.
        auto* anchor = scroll_->widget()->childAt(scroll_->widget()->width() / 2, scroll_->verticalScrollBar()->value() + 1);
        while (anchor && anchor != scroll_->widget() && !anchor->property("choice").isValid())
        {
            anchor = anchor->parentWidget();
        }
        auto offset = anchor ? anchor->mapTo(scroll_->widget(), QPoint{}).y() - scroll_->verticalScrollBar()->value() : 0;
        QHash<qint64, QPushButton*> existing;
        while (auto* item = g.rows->takeAt(0))
        {
            auto* row = static_cast<QPushButton*>(item->widget());
            existing.insert(row->property("user").toLongLong(), row);
            delete item;
        }
        for (int i = 0; i < std::min<int>(g.limit(), g.members.size()); ++i)
        {
            auto const& person = g.members[i];
            auto* row = existing.take(person.id);
            auto name = QString::fromStdString(person.nickname.empty() ? person.username : person.nickname);
            if (!row)
            {
                row = new QPushButton(name);
                row->setFlat(true);
                row->setObjectName("pollVoter");
                row->setProperty("choice", choice);
                row->setProperty("user", qint64(person.id));
                row->setToolTip(QString::fromStdString(person.username));
                row->setIconSize({28, 28});
                auto refresh = [this, row, id = person.id] { row->setIcon(avatar_icon(row->text(), 28, chat_.avatars().image(id))); };

                connect(&chat_.avatars(),
                        &avatar_cache::changed,
                        row,
                        [refresh, id = person.id](qint64 changed)
                        {
                            if (changed == id)
                            {
                                refresh();
                            }
                        });
                connect(row, &QPushButton::clicked, this, [this, id = person.id, row] { chat_.show_user_details(id, row->text(), this); });
            }
            row->setText(name);
            chat_.avatars().observe(person.id, person.avatar);
            row->setIcon(avatar_icon(name, 28, chat_.avatars().image(person.id)));
            g.rows->addWidget(row);
        }
        for (auto* removed : existing)
        {
            removed->hide();
            removed->deleteLater();
        }
        update_controls(g);
        column_->activate();
        if (anchor && anchor != scroll_->widget() && anchor->isVisible())
        {
            scroll_->verticalScrollBar()->setValue(anchor->mapTo(scroll_->widget(), QPoint{}).y() - offset);
        }
    }

    voter_group make_group(qint64 choice)
    {
        auto* section = new QWidget;
        section->setProperty("choice", choice);
        auto* box = new QVBoxLayout(section);
        box->setContentsMargins(0, 8, 0, 4);
        auto* line = new QHBoxLayout;
        auto* title = new QLabel;
        title->setObjectName("pollGroup");
        title->setWordWrap(true);
        title->setTextFormat(Qt::PlainText);
        line->addWidget(title, 1);
        auto* tally = new QLabel;
        tally->setObjectName("pollTally");
        line->addWidget(tally);
        auto* collapse = new QPushButton(QStringLiteral("收起"));
        collapse->setFlat(true);
        collapse->setObjectName("pollCollapse");
        collapse->setProperty("choice", choice);
        line->addWidget(collapse);
        box->addLayout(line);
        auto* rows = new QVBoxLayout;
        box->addLayout(rows);
        auto* more = new QPushButton;
        more->setFlat(true);
        more->setProperty("choice", choice);
        more->setObjectName("pollMore");
        box->addWidget(more);
        // Zero-vote options may acquire their first voter in a later revision.
        int at = 2;
        for (auto it = groups_.cbegin(); it != groups_.cend(); ++it)
        {
            if (it.key() < choice)
            {
                ++at;
            }
        }
        column_->insertWidget(at, section);
        connect(more,
                &QPushButton::clicked,
                this,
                [this, choice]
                {
                    auto& g = groups_[choice];
                    if (g.request)
                    {
                        return;
                    }
                    if (g.more->property("retry").toBool())
                    {
                        g.more->setProperty("retry", false);
                    }
                    else
                    {
                        g.wanted = std::min<qint64>(g.votes, g.limit() + 50);
                        if (!g.after || g.members.size() >= g.wanted)
                        {
                            populate(g, choice);
                            return;
                        }
                        populate(g, choice);
                    }
                    request(choice);
                });
        connect(collapse,
                &QPushButton::clicked,
                this,
                [this, choice]
                {
                    auto& g = groups_[choice];
                    g.wanted = 0;
                    populate(g, choice);
                    scroll_->verticalScrollBar()->setValue(g.section->mapTo(scroll_->widget(), QPoint{}).y());
                });
        return {section, title, tally, collapse, rows, more};
    }

    void render(chat::poll_summary const& poll)
    {
        if (poll.revision <= revision_)
        {
            return;
        }
        if (!has_poll_results(poll))
        {
            back_(this);
            return;
        }
        auto previous = std::exchange(revision_, poll.revision);
        status_->setText(
            QStringLiteral("%1 · 共 %2 人参与").arg(poll.closed ? QStringLiteral("投票已结束") : QStringLiteral("实名投票")).arg(poll.participants));
        auto percentages = poll_percentages(poll);
        for (const auto& option : poll.options)
        {
            auto choice = option.position;
            if (!groups_.contains(choice))
            {
                if (!option.votes)
                {
                    continue;
                }
                groups_.insert(choice, make_group(choice));
            }
            auto& g = groups_[choice];
            g.section->setVisible(option.votes != 0);
            g.title->setText(QStringLiteral("%1%   %2").arg(percentages[choice - 1]).arg(QString::fromStdString(option.label)));
            g.tally->setText(QStringLiteral("%1 票").arg(option.votes));
            if (previous + 1 == revision_ && g.votes == option.votes)
            {
                continue;
            }
            g.votes = option.votes;
            g.request = 0;
            g.after = 0;
            g.members.clear();
            if (!g.votes)
            {
                populate(g, choice);
                continue;
            }

            request(choice);
        }
    }

    void received(quint64 req, qint64 id, qint64 choice, qint64 after, chat::poll_voters page, QString error)
    {
        auto it = groups_.find(choice);
        if (id != message_ || it == groups_.end() || it->request != req || it->after != after)
        {
            return;
        }
        auto& g = *it;
        g.request = 0;
        if (!error.isEmpty())
        {
            g.more->setText(QStringLiteral("重试加载成员"));
            g.more->setProperty("retry", true);
            g.more->setEnabled(true);
            g.more->show();
            return;
        }
        for (auto& person : page.members)
        {
            g.members.append(std::move(person));
        }
        g.after = page.next;
        if (g.after && g.members.size() < g.limit())
        {
            request(choice);
            return;
        }
        populate(g, choice);
    }
    client_bridge& client_;
    chat_widget& chat_;
    qint64 room_, message_;
    std::function<void(QWidget*)> back_;
    QScrollArea* scroll_;
    QVBoxLayout* column_;
    QLabel* status_;
    quint64 query_ = 0;
    qint64 revision_ = 0;
    QHash<qint64, voter_group> groups_;
};
}    // namespace

void chat_widget::open_poll_results(client_bridge& client, qint64 room, qint64 message)
{
    if (room != active_conversation_ || message <= history_floor(room))
    {
        return;
    }
    auto index = messages_->message_index(message);
    auto poll = index.data(message_model::poll_role).value<chat::poll_summary>();
    if (!has_poll_results(poll))
    {
        return;
    }
    close_poll_results();
    poll_return_focus_ = focusWidget();
    auto* page = new results_page(client,
                                  *this,
                                  room,
                                  message,
                                  index.data(message_model::text_role).toString(),
                                  poll,
                                  [this](QWidget* page)
                                  {
                                      if (page == poll_page_)
                                      {
                                          close_poll_results();
                                      }
                                  });
    poll_page_ = page;
    static_cast<QHBoxLayout*>(layout())->addWidget(page);
    layout_poll_results();
    page->show();
    page->setFocus();
}

void chat_widget::layout_poll_results()
{
    if (!poll_page_)
    {
        return;
    }
    bool wide = width() >= 1100;
    timeline_->setVisible(wide);
    poll_page_->setMinimumWidth(wide ? 300 : 0);
    poll_page_->setMaximumWidth(wide ? 320 : QWIDGETSIZE_MAX);
    static_cast<QHBoxLayout*>(layout())->setStretchFactor(poll_page_, wide ? 0 : 1);
}

void chat_widget::close_poll_results()
{
    if (!poll_page_)
    {
        return;
    }
    // The back callback is scoped to its own page, including while deletion is deferred.
    auto* old = std::exchange(poll_page_, nullptr);
    const bool restore_focus = old->hasFocus() || old->isAncestorOf(focusWidget());
    old->hide();
    layout()->removeWidget(old);
    old->deleteLater();
    timeline_->show();
    if (restore_focus)
    {
        auto* target = poll_return_focus_.data();
        (target && target->isVisible() ? target : messages_view_)->setFocus();
    }
    poll_return_focus_.clear();
    mark_visible_messages();
}
