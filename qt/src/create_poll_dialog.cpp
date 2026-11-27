#include "create_poll_dialog.hpp"
#include "chat_widget.hpp"
#include "client_bridge.hpp"
#include "theme.hpp"
#include <QButtonGroup>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

namespace
{
class option_edit final : public QLineEdit
{
   protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        {
            emit returnPressed();
            event->accept();
            return;
        }
        QLineEdit::keyPressEvent(event);
    }
};

class option_handle final : public QLabel
{
   public:
    option_handle(QWidget* row, QVBoxLayout* list, std::function<void()> changed)
        : QLabel(QStringLiteral("≡"), row), list_(list), changed_(std::move(changed))
    {
        setObjectName("pollHandle");
        setCursor(Qt::OpenHandCursor);
        setToolTip(QStringLiteral("拖动调整选项顺序"));
        setMargin(5);
    }

   private:
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (e->buttons() & Qt::LeftButton)
        {
            auto y = list_->parentWidget()->mapFromGlobal(e->globalPosition().toPoint()).y();
            auto* row = parentWidget();
            if (row->findChild<QLineEdit*>()->text().trimmed().isEmpty())
            {
                return;
            }
            int from = list_->indexOf(row);
            for (int i = 0; i < list_->count(); ++i)
            {
                if (list_->itemAt(i)->widget()->findChild<QLineEdit*>()->text().trimmed().isEmpty())
                {
                    continue;
                }
                auto rect = list_->itemAt(i)->geometry();
                if ((i < from && y <= rect.center().y()) || (i > from && y >= rect.center().y() && y <= rect.bottom()))
                {
                    list_->removeWidget(row);
                    list_->insertWidget(i, row);
                    changed_();
                    break;
                }
            }
        }
    }
    QVBoxLayout* list_;
    std::function<void()> changed_;
};

class poll_form final : public QDialog
{
   public:
    using QDialog::QDialog;
    std::function<bool()> discard;
    void reject() override
    {
        if (!discard || discard())
        {
            QDialog::reject();
        }
    }
};
}    // namespace

void compose_poll(client_bridge& client, chat_widget& chat, qint64 room)
{
    poll_form dialog(&chat);
    dialog.setObjectName("pollForm");
    dialog.setWindowTitle(QStringLiteral("发起群投票"));
    auto* outer = new QVBoxLayout(&dialog);
    frame_dialog(dialog);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* panel = new QWidget;
    auto* layout = new QVBoxLayout(panel);
    layout->setAlignment(Qt::AlignTop);
    scroll->setWidget(panel);
    outer->addWidget(scroll, 1);
    auto title = [&](QString text)
    {
        auto* label = new QLabel(text);
        auto font = label->font();
        font.setBold(true);
        label->setFont(font);
        layout->addWidget(label);
    };
    auto editor = [&](QString name, QString placeholder)
    {
        auto* edit = new QPlainTextEdit;
        edit->setObjectName(name);
        edit->setPlaceholderText(placeholder);
        edit->setTabChangesFocus(true);
        edit->setFixedHeight(edit->fontMetrics().height() * 2 + 16);
        layout->addWidget(edit);
        return edit;
    };
    title(QStringLiteral("投票问题"));
    auto* question = editor("pollQuestion", QStringLiteral("提出问题"));
    title(QStringLiteral("选项"));
    auto* options = new QWidget;
    layout->addWidget(options);
    auto* rows = new QVBoxLayout(options);
    rows->setContentsMargins(0, 0, 0, 0);
    auto* remaining = new QLabel;
    layout->addWidget(remaining);
    title(QStringLiteral("设置"));
    auto toggle = [&](QString text, QString name)
    {
        auto* box = new QCheckBox(text);
        box->setObjectName(name);
        layout->addWidget(box);
        return box;
    };
    auto* anonymous = toggle(QStringLiteral("匿名投票"), "pollAnonymous");
    anonymous->setChecked(true);
    auto* multiple = toggle(QStringLiteral("允许多选"), "pollMultiple");
    auto* quiz = toggle(QStringLiteral("问答模式"), "pollQuiz");
    auto* explanation = editor("pollExplanation", QStringLiteral("答案解析（选填，最多 200 个字符）"));
    auto* feedback = new QLabel;
    feedback->setWordWrap(true);
    feedback->setObjectName("pollFeedback");
    outer->addWidget(feedback);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto* ok = buttons->button(QDialogButtonBox::Ok);
    ok->setText(QStringLiteral("创建"));
    ok->setObjectName("pollSubmit");
    ok->setProperty("primary", true);
    ok->setAutoDefault(false);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    outer->addWidget(buttons);
    QButtonGroup answers(&dialog);
    qint64 request_id = 0;
    bool sending = false;
    bool available = true;
    bool membership_verified = true;
    auto read = [&](QStringList& labels, chat::poll_settings& settings)
    {
        settings.anonymous = anonymous->isChecked();
        settings.multiple = multiple->isChecked();
        bool duplicate = false, tooLong = false;
        for (int i = 0; i < rows->count(); ++i)
        {
            auto* row = rows->itemAt(i)->widget();
            auto label = row->findChild<QLineEdit*>()->text().trimmed();
            if (label.isEmpty())
            {
                continue;
            }
            duplicate |= labels.contains(label);
            tooLong |= label.toUcs4().size() > 80;
            labels.append(label);
            if (quiz->isChecked() && row->findChild<QRadioButton*>()->isChecked())
            {
                settings.correct = labels.size();
            }
        }
        if (quiz->isChecked())
        {
            settings.explanation = explanation->toPlainText().trimmed().toStdString();
        }
        auto q = question->toPlainText().trimmed();
        if (q.isEmpty())
        {
            return QStringLiteral("请输入投票问题");
        }
        if (q.toUcs4().size() > 256)
        {
            return QStringLiteral("问题最多可写 256 个字符");
        }
        if (duplicate)
        {
            return QStringLiteral("选项不能重复");
        }
        if (tooLong)
        {
            return QStringLiteral("每个选项最多可写 80 个字符");
        }
        if (labels.size() < 2)
        {
            return QStringLiteral("至少填写两个选项");
        }
        if (quiz->isChecked() && !settings.correct)
        {
            return QStringLiteral("请选择正确答案");
        }
        if (quiz->isChecked() && explanation->toPlainText().trimmed().toUcs4().size() > 200)
        {
            return QStringLiteral("解析最多可写 200 个字符");
        }
        return QString{};
    };
    auto tab_order = [&]
    {
        QWidget* previous = question;
        for (int i = 0; i < rows->count(); ++i)
        {
            auto* field = rows->itemAt(i)->widget()->findChild<QLineEdit*>();
            QWidget::setTabOrder(previous, field);
            previous = field;
        }
        QWidget::setTabOrder(previous, anonymous);
    };
    auto validate = [&]
    {
        QStringList labels;
        chat::poll_settings settings;
        auto error = read(labels, settings);
        feedback->setText(error);
        ok->setEnabled(membership_verified && available && !sending && error.isEmpty());
        ok->setText(request_id && !sending ? QStringLiteral("重试创建") : QStringLiteral("创建"));
        panel->setEnabled(!request_id);
        buttons->button(QDialogButtonBox::Cancel)->setEnabled(!sending);
        if (sending)
        {
            feedback->setText(QStringLiteral("正在创建投票…"));
        }
        else if (request_id)
        {
            feedback->setText(ok->isEnabled() ? QStringLiteral("创建结果尚未确认，可重试原请求")
                                              : QStringLiteral("创建结果尚未确认，恢复发送权限后可重试"));
        }
        else if (!membership_verified || !available)
        {
            feedback->setText(QStringLiteral("当前无法发起投票，已保留输入"));
        }
        else if (error.isEmpty())
        {
            feedback->setText(QStringLiteral("问题还可输入 %1 字").arg(256 - question->toPlainText().trimmed().toUcs4().size()));
        }
        remaining->setText(QStringLiteral("还可添加 %1 个选项").arg(10 - labels.size()));
        remaining->setVisible(labels.size() < 10);
        explanation->setVisible(quiz->isChecked());
        for (int i = 0; i < rows->count(); ++i)
        {
            auto* row = rows->itemAt(i)->widget();
            row->findChild<QRadioButton*>()->setVisible(quiz->isChecked() && !row->findChild<QLineEdit*>()->text().trimmed().isEmpty());
            row->findChild<QToolButton*>()->setEnabled(rows->count() > 1);
        }
    };
    std::function<void()> append;
    auto ensure_tail = [&]
    {
        while (rows->count() > 1)
        {
            auto* last = rows->itemAt(rows->count() - 1)->widget();
            auto* before = rows->itemAt(rows->count() - 2)->widget()->findChild<QLineEdit*>();
            if (!last->findChild<QLineEdit*>()->text().trimmed().isEmpty() || !before->text().trimmed().isEmpty())
            {
                break;
            }
            if (last->isAncestorOf(dialog.focusWidget()))
            {
                before->setFocus();
            }
            rows->removeWidget(last);
            last->hide();
            last->setParent(nullptr);
            last->deleteLater();
        }
        if (rows->count() < 10 && (!rows->count() || !rows->itemAt(rows->count() - 1)->widget()->findChild<QLineEdit*>()->text().trimmed().isEmpty()))
        {
            append();
        }
        tab_order();
        validate();
    };
    append = [&]
    {
        auto* row = new QWidget(options);
        auto* line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        auto* correct = new QRadioButton;
        correct->setToolTip(QStringLiteral("设为正确答案"));
        answers.addButton(correct);
        line->addWidget(correct);
        auto* edit = new option_edit;
        edit->setObjectName("pollOption");
        edit->setPlaceholderText(QStringLiteral("填写选项"));
        line->addWidget(edit, 1);
        auto* remove = new QToolButton;
        remove->setObjectName("pollRemove");
        remove->setText(QStringLiteral("×"));
        remove->setToolTip(QStringLiteral("移除选项"));
        line->addWidget(remove);
        line->addWidget(new option_handle(row, rows, tab_order));
        rows->addWidget(row);
        QObject::connect(edit, &QLineEdit::textChanged, &dialog, ensure_tail);
        QObject::connect(correct, &QRadioButton::toggled, &dialog, validate);
        QObject::connect(remove,
                         &QToolButton::clicked,
                         &dialog,
                         [&, row]
                         {
                             int i = rows->indexOf(row);
                             rows->removeWidget(row);
                             row->hide();
                             row->deleteLater();
                             ensure_tail();
                             rows->itemAt(std::min(i, rows->count() - 1))->widget()->findChild<QLineEdit*>()->setFocus();
                         });
        QObject::connect(edit,
                         &QLineEdit::returnPressed,
                         &dialog,
                         [&, row]
                         {
                             auto next = rows->indexOf(row) + 1;
                             if (next < rows->count())
                             {
                                 rows->itemAt(next)->widget()->findChild<QLineEdit*>()->setFocus();
                             }
                             else
                             {
                                 ok->setFocus();
                             }
                         });
    };
    ensure_tail();
    QObject::connect(quiz,
                     &QCheckBox::toggled,
                     &dialog,
                     [&](bool on)
                     {
                         if (on)
                         {
                             multiple->setChecked(false);
                         }
                         validate();
                     });
    QObject::connect(multiple,
                     &QCheckBox::toggled,
                     &dialog,
                     [&](bool on)
                     {
                         if (on)
                         {
                             quiz->setChecked(false);
                         }
                         validate();
                     });
    QObject::connect(question, &QPlainTextEdit::textChanged, &dialog, validate);
    QObject::connect(explanation, &QPlainTextEdit::textChanged, &dialog, validate);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.discard = [&]
    {
        if (request_id)
        {
            return !sending &&
                   confirm_action(
                       &dialog, QStringLiteral("关闭投票表单"), QStringLiteral("这次创建尚未确认，投票可能已经发送。"), QStringLiteral("关闭"));
        }
        QStringList labels;
        chat::poll_settings settings;
        read(labels, settings);
        return ((question->toPlainText().isEmpty() && labels.isEmpty() && explanation->toPlainText().isEmpty()) ||
                confirm_action(&dialog, QStringLiteral("放弃投票草稿"), QStringLiteral("关闭后将丢弃已填写的问题和选项。"), QStringLiteral("放弃")));
    };
    QObject::connect(buttons,
                     &QDialogButtonBox::accepted,
                     &dialog,
                     [&]
                     {
                         QStringList labels;
                         chat::poll_settings settings;
                         if (!membership_verified || !available || sending || !read(labels, settings).isEmpty() || chat.active_conversation() != room)
                         {
                             return;
                         }
                         if (!request_id)
                         {
                             request_id = chat::client::new_client_id();
                         }
                         sending = true;
                         validate();
                         client.create_poll(room, request_id, question->toPlainText().trimmed(), labels, std::move(settings));
                     });
    QObject::connect(&client,
                     &client_bridge::poll_created,
                     &dialog,
                     [&](qint64 request, QString error, bool refused)
                     {
                         if (request != request_id)
                         {
                             return;
                         }
                         if (error.isEmpty())
                         {
                             if (chat.active_conversation() == room)
                             {
                                 chat.return_to_latest();
                             }
                             dialog.accept();
                             return;
                         }
                         sending = false;
                         if (refused)
                         {
                             request_id = 0;
                         }
                         validate();
                         feedback->setText(refused ? error : QStringLiteral("%1。创建结果尚未确认，可重试原请求。").arg(error));
                     });
    QObject::connect(&client,
                     &client_bridge::disconnected,
                     &dialog,
                     [&]
                     {
                         membership_verified = available = sending = false;
                         validate();
                     });
    QObject::connect(&client,
                     &client_bridge::members_received,
                     &dialog,
                     [&](qint64 current, const QList<member_data>& members, const QString& error)
                     {
                         if (current == room && !membership_verified && error.isEmpty())
                         {
                             membership_verified = std::ranges::find(members, chat.self_user(), &member_data::id) != members.end();
                             if (!membership_verified)
                             {
                                 dialog.done(QDialog::Rejected);
                                 return;
                             }
                             validate();
                         }
                     });
    QObject::connect(&chat,
                     &chat_widget::poll_creation_availability,
                     &dialog,
                     [&](qint64 current, bool allowed)
                     {
                         if (current != room)
                         {
                             dialog.done(QDialog::Rejected);
                             return;
                         }
                         if (available != allowed)
                         {
                             available = allowed;
                             validate();
                         }
                     });
    QObject::connect(&chat,
                     &chat_widget::conversation_selected,
                     &dialog,
                     [&](qint64 selected, bool)
                     {
                         if (selected != room)
                         {
                             dialog.done(QDialog::Rejected);
                         }
                     });
    question->setFocus();
    dialog.resize(440, 570);
    dialog.exec();
    dialog.discard = {};
}
