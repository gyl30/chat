#include "poll_dialog.hpp"
#include "theme.hpp"
#include <QComboBox>
#include "chat_widget.hpp"
#include "client_bridge.hpp"
#include <QDialogButtonBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QSet>
#include <QMessageBox>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QTextLayout>
#include <QScreen>
#include "theme_manager.hpp"

namespace
{
enum option_role
{
    count_role = Qt::UserRole + 1,
    fraction_role,
    mine_role
};

class option_delegate final : public QStyledItemDelegate
{
   public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const override
    {
        auto const* list = static_cast<QListWidget const*>(parent());
        int width = list->viewport()->width();
        return {width, render_label(index, option.font, width - 40, nullptr, {}) + 38};
    }

    void paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        auto row = option.rect.adjusted(2, 2, -2, -2);
        bool selected = option.state & QStyle::State_Selected;
        painter->setBrush(themed(option.state & QStyle::State_MouseOver ? "canvas_hover" : "canvas"));
        painter->setPen(themed(selected ? "focus" : "border"));
        painter->drawRoundedRect(row, 6, 6);
        QPoint center(row.left() + 13, row.top() + 8 + option.fontMetrics.height() / 2);
        painter->drawEllipse(center, 6, 6);
        if (selected)
        {
            painter->setBrush(themed("focus"));
            painter->drawEllipse(center, 3, 3);
        }
        int left = row.left() + 26;
        painter->setPen(themed("text"));
        render_label(index, option.font, option.rect.width() - 40, painter, {left, row.top() + 5});
        double fraction = index.data(fraction_role).toDouble();
        QString stats = QStringLiteral("%1 票 · %2%").arg(index.data(count_role).toLongLong()).arg(fraction * 100, 0, 'f', fraction == 0 ? 0 : 1);
        auto detail = row.adjusted(26, 0, -10, -7);
        painter->setPen(themed("text_secondary"));
        painter->drawText(detail, Qt::AlignRight | Qt::AlignBottom, stats);
        if (index.data(mine_role).toBool())
        {
            painter->setPen(themed("accent"));
            painter->drawText(detail, Qt::AlignLeft | Qt::AlignBottom, QStringLiteral("我的投票"));
        }
        QRectF track(left, row.bottom() - 4, detail.width(), 3);
        painter->setPen(Qt::NoPen);
        painter->setBrush(themed("border"));
        painter->drawRoundedRect(track, 1.5, 1.5);
        painter->setBrush(themed("accent"));
        track.setWidth(track.width() * fraction);
        painter->drawRoundedRect(track, 1.5, 1.5);
        painter->restore();
    }

   private:
    static int render_label(QModelIndex const& index, QFont const& font, int width, QPainter* painter, QPoint origin)
    {
        QTextLayout layout(index.data().toString(), font);
        QTextOption wrap;
        wrap.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(wrap);
        layout.beginLayout();
        qreal height = 0;
        for (auto line = layout.createLine(); line.isValid(); line = layout.createLine())
        {
            line.setLineWidth(std::max(1, width));
            line.setPosition({0, height});
            height += line.height();
        }
        layout.endLayout();
        if (painter)
        {
            layout.draw(painter, origin);
        }
        return qCeil(height);
    }
};
}    // namespace

std::optional<std::pair<QString, QStringList>> compose_poll(QWidget* parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("发起群投票"));
    auto* layout = new QVBoxLayout(&dialog);
    frame_dialog(dialog);
    auto* question = new QLineEdit(&dialog);
    question->setPlaceholderText(QStringLiteral("投票问题（最多 256 字）"));
    auto* options = new QPlainTextEdit(&dialog);
    options->setPlaceholderText(QStringLiteral("每行一个选项，2 至 10 项，每项最多 80 字"));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    dialog.resize(420, 330);
    auto* prompt = new QLabel(QStringLiteral("投票问题"));
    prompt->setBuddy(question);
    auto* labels = new QLabel(QStringLiteral("投票选项 · 每行一项"));
    labels->setBuddy(options);
    auto* feedback = new QLabel;
    feedback->setObjectName(QStringLiteral("pollFeedback"));
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("发起投票"));
    buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("pollSubmit"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    for (auto* button : buttons->buttons())
    {
        button->setIcon({});
        button->style()->unpolish(button);
        button->style()->polish(button);
    }
    layout->addWidget(prompt);
    layout->addWidget(question);
    layout->addWidget(labels);
    layout->addWidget(options);
    layout->addWidget(feedback);
    layout->addWidget(buttons);
    auto values = [&]
    {
        auto labels = options->toPlainText().trimmed().split(u'\n');
        for (auto& label : labels)
        {
            label = label.trimmed();
        }
        return labels;
    };
    auto validate = [&]
    {
        auto q = question->text().trimmed();
        auto labels = values();
        QSet<QString> seen;
        QString error;
        if (q.isEmpty())
        {
            error = QStringLiteral("请输入投票问题");
        }
        else if (q.toUcs4().size() > 256)
        {
            error = QStringLiteral("问题最多可写 256 个字符");
        }
        else if (labels.size() < 2 || labels.size() > 10)
        {
            error = QStringLiteral("请填写 2 至 10 个选项");
        }
        else
        {
            for (auto const& label : labels)
            {
                if (label.isEmpty())
                {
                    error = QStringLiteral("选项不能为空行");
                }
                else if (label.toUcs4().size() > 80)
                {
                    error = QStringLiteral("每个选项最多可写 80 个字符");
                }
                else if (seen.contains(label))
                {
                    error = QStringLiteral("选项不能重复");
                }
                if (!error.isEmpty())
                {
                    break;
                }
                seen.insert(label);
            }
        }
        feedback->setText(error.isEmpty() ? QStringLiteral("实名单选 · 共 %1 项").arg(labels.size()) : error);
        buttons->button(QDialogButtonBox::Ok)->setEnabled(error.isEmpty());
    };
    QObject::connect(question, &QLineEdit::textChanged, &dialog, validate);
    QObject::connect(options, &QPlainTextEdit::textChanged, &dialog, validate);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    validate();
    if (dialog.exec() != QDialog::Accepted)
    {
        return std::nullopt;
    }
    return std::pair{question->text().trimmed(), values()};
}

void show_poll(client_bridge& client, chat_widget& chat, qint64 room, qint64 message, QWidget* parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("群投票"));
    auto* layout = new QVBoxLayout(&dialog);
    frame_dialog(dialog);
    auto* question = new QLabel(&dialog);
    question->setWordWrap(true);
    question->setTextFormat(Qt::PlainText);
    auto* options = new QListWidget(&dialog);
    auto* status = new QLabel(&dialog);
    status->setTextFormat(Qt::PlainText);
    auto* actions = new QHBoxLayout;
    auto* submit = new QPushButton(QStringLiteral("提交投票"), &dialog);
    auto* cancel = new QPushButton(QStringLiteral("取消我的投票"), &dialog);
    auto* close = new QPushButton(QStringLiteral("结束投票"), &dialog);
    auto* people = new QListWidget(&dialog);
    auto* more = new QPushButton(QStringLiteral("更多投票成员"), &dialog);
    dialog.resize(500, 430);
    layout->setSpacing(9);
    options->setItemDelegate(new option_delegate(options));
    options->setMouseTracking(true);
    options->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    people->setSelectionMode(QAbstractItemView::NoSelection);
    people->setFocusPolicy(Qt::NoFocus);
    question->setObjectName(QStringLiteral("dialogTitle"));
    status->setObjectName(QStringLiteral("pollSummary"));
    submit->setObjectName(QStringLiteral("pollSubmit"));
    options->setWordWrap(true);
    options->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    options->setResizeMode(QListView::Adjust);
    auto* voter_option = new QComboBox;
    voter_option->setMinimumContentsLength(8);
    voter_option->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    people->setMinimumHeight(48);
    auto* viewer = new QHBoxLayout;
    auto* viewer_label = new QLabel(QStringLiteral("查看投票成员"));
    viewer_label->setBuddy(voter_option);
    viewer->addWidget(viewer_label);
    viewer->addWidget(voter_option, 1);
    layout->addWidget(question);
    layout->addWidget(status);
    layout->addWidget(options);
    layout->addLayout(viewer);
    layout->addWidget(people, 1);
    layout->addWidget(more);
    layout->addLayout(actions);
    actions->addWidget(close);
    actions->addStretch();
    actions->addWidget(cancel);
    actions->addWidget(submit);
    qint64 cursor = 0;
    quint64 query = 0, operation = 0, voters_query = 0;
    chat::poll_state state;
    auto controls = [&]
    {
        bool writable = !operation && !query && !state.summary.closed;
        submit->setText(state.selection ? QStringLiteral("修改投票") : QStringLiteral("提交投票"));
        submit->setEnabled(writable && options->currentRow() >= 0 && options->currentRow() + 1 != state.selection);
        submit->setVisible(!state.summary.closed);
        cancel->setVisible(state.selection && !state.summary.closed);
        cancel->setEnabled(writable);
        close->setEnabled(writable);
        auto room_state = chat.conversation(room);
        close->setVisible(state.can_close && room_state && room_state->speech_role != chat::member_role::member && !state.summary.closed);
    };
    QObject::connect(&client, &client_bridge::conversations_received, &dialog, controls);
    auto reload = [&]
    {
        voters_query = 0;
        more->setEnabled(false);
        query = client.get_poll(room, message);
        controls();
    };
    auto load_people = [&]
    {
        voters_query = 0;
        voter_option->setToolTip(voter_option->currentText());
        cursor = 0;
        more->hide();
        people->clear();
        controls();
        if (voter_option->currentIndex() >= 0)
        {
            voters_query = client.get_poll_voters(room, message, voter_option->currentIndex() + 1, 0);
        }
    };
    QObject::connect(options, &QListWidget::currentRowChanged, &dialog, controls);
    QObject::connect(voter_option, &QComboBox::currentIndexChanged, &dialog, load_people);
    QObject::connect(&client,
                     &client_bridge::poll_received,
                     &dialog,
                     [&](quint64 request, qint64 r, qint64 id, chat::poll_state value, QString err)
                     {
                         if (r != room || id != message)
                         {
                             return;
                         }
                         if (request == operation)
                         {
                             operation = 0;
                             controls();
                             if (!err.isEmpty())
                             {
                                 status->setText(err);
                             }
                             // Successful mutations also publish poll_changed. That refresh reads
                             // the current selection and permissions, even after a dialog was reopened.
                             return;
                         }
                         if (request != query)
                         {
                             return;
                         }
                         query = 0;
                         if (!err.isEmpty())
                         {
                             status->setText(err);
                             submit->setEnabled(false);
                             cancel->setEnabled(false);
                             close->hide();
                             return;
                         }
                         if (value.summary.revision < state.summary.revision)
                         {
                             return;
                         }
                         auto prev = options->currentRow();
                         state = std::move(value);
                         question->setText(QString::fromStdString(state.question));
                         qint64 total = 0;
                         for (auto const& option : state.summary.options)
                         {
                             total += option.votes;
                         }
                         {
                             QSignalBlocker blocker(options);
                             QSignalBlocker viewing(voter_option);
                             int viewed = voter_option->currentIndex();
                             voter_option->clear();
                             options->clear();
                             for (auto const& option : state.summary.options)
                             {
                                 voter_option->addItem(QString::fromStdString(option.label));
                                 auto* item = new QListWidgetItem(QString::fromStdString(option.label), options);
                                 auto fraction = total ? static_cast<double>(option.votes) / total : 0.;
                                 bool mine = state.selection == option.position;
                                 item->setData(count_role, qlonglong(option.votes));
                                 item->setData(fraction_role, fraction);
                                 item->setData(mine_role, mine);
                                 item->setData(Qt::AccessibleTextRole,
                                               QStringLiteral("%1 · %2 票%3")
                                                   .arg(item->text())
                                                   .arg(option.votes)
                                                   .arg(mine ? QStringLiteral(" · 我的投票") : QString{}));
                             }
                             options->setCurrentRow(prev >= 0 ? prev : state.selection ? int(state.selection) - 1 : 0);
                             voter_option->setCurrentIndex(viewed >= 0 ? viewed : options->currentRow());
                             int height = 2 * options->frameWidth() + 4;
                             for (int i = 0; i < options->count(); ++i)
                             {
                                 height += options->sizeHintForRow(i);
                             }
                             options->setFixedHeight(
                                 qBound(64, height, std::min(370, std::max(150, dialog.screen()->availableGeometry().height() - 290))));
                             if (prev < 0)
                             {
                                 dialog.resize(dialog.width(), options->height() + 255);
                             }
                             options->setSelectionMode(state.summary.closed ? QAbstractItemView::NoSelection : QAbstractItemView::SingleSelection);
                         }
                         status->setText(QStringLiteral("实名单选 · %1 · 共 %2 票")
                                             .arg(state.summary.closed ? QStringLiteral("投票已结束") : QStringLiteral("进行中"))
                                             .arg(total));
                         load_people();
                     });
    QObject::connect(&client,
                     &client_bridge::poll_changed,
                     &dialog,
                     [&](chat::poll_update const& update)
                     {
                         if (update.message == message && update.summary.revision > state.summary.revision)
                         {
                             reload();
                         }
                     });
    QObject::connect(
        &client,
        &client_bridge::voters_received,
        &dialog,
        [&](quint64 request, qint64 id, qint64 choice, qint64 after, chat::poll_voters page, QString err)
        {
            if (request != voters_query || id != message || choice != voter_option->currentIndex() + 1 || after != cursor)
            {
                return;
            }
            if (!err.isEmpty())
            {
                status->setText(err);
                return;
            }
            if (!after)
            {
                people->clear();
            }
            for (auto const& member : page.members)
            {
                auto name = QString::fromStdString(member.username);
                people->addItem(member.nickname.empty() ? name : QStringLiteral("%1 (@%2)").arg(QString::fromStdString(member.nickname), name));
            }
            cursor = page.next;
            more->setVisible(cursor > 0);
            more->setEnabled(true);
        });
    QObject::connect(more,
                     &QPushButton::clicked,
                     &dialog,
                     [&]
                     {
                         more->setEnabled(false);
                         voters_query = client.get_poll_voters(room, message, voter_option->currentIndex() + 1, cursor);
                     });
    QObject::connect(submit,
                     &QPushButton::clicked,
                     &dialog,
                     [&]
                     {
                         operation = client.vote_poll(room, message, options->currentRow() + 1);
                         controls();
                     });
    QObject::connect(cancel,
                     &QPushButton::clicked,
                     &dialog,
                     [&]
                     {
                         operation = client.vote_poll(room, message, 0);
                         controls();
                     });
    QObject::connect(
        close,
        &QPushButton::clicked,
        &dialog,
        [&]
        {
            if (!confirm_action(
                    &dialog, QStringLiteral("结束投票"), QStringLiteral("结束后将保留结果，成员无法继续投票或改票。"), QStringLiteral("结束投票")) ||
                !dialog.isVisible() || !close->isEnabled() || !close->isVisible())
            {
                return;
            }
            operation = client.close_poll(room, message);
            controls();
        });
    QObject::connect(&chat,
                     &chat_widget::history_discarded,
                     &dialog,
                     [&](qint64 id, qint64 through)
                     {
                         if (id == room && message <= through)
                         {
                             dialog.reject();
                         }
                     });
    QObject::connect(&client,
                     &client_bridge::conversation_changed,
                     &dialog,
                     [&](qint64 id, bool removed)
                     {
                         if (id == room)
                         {
                             if (removed)
                             {
                                 dialog.reject();
                             }
                             else
                             {
                                 reload();
                             }
                         }
                     });
    QObject::connect(&client,
                     &client_bridge::message_updated,
                     &dialog,
                     [&](qint64 id, message_data m, QString)
                     {
                         if (id == room && m.id == message && m.deleted)
                         {
                             dialog.reject();
                         }
                     });
    QObject::connect(&client, &client_bridge::disconnected, &dialog, &QDialog::reject);
    QObject::connect(&dialog,
                     &QDialog::finished,
                     &dialog,
                     [&dialog]
                     {
                         if (auto* confirm = dialog.findChild<QMessageBox*>(QStringLiteral("confirmationDialog")))
                         {
                             confirm->reject();
                         }
                     });
    controls();
    more->hide();
    reload();
    dialog.exec();
}
