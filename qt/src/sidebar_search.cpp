#include "sidebar_search.hpp"

#include "avatar.hpp"
#include "client_bridge.hpp"
#include "conversation_model.hpp"
#include "user_model.hpp"
#include <QDateTime>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

sidebar_search::sidebar_search(client_bridge& client, QLineEdit& input, user_model& contacts, conversation_model& rooms, avatar_cache& avatars, QWidget* parent)
    : QWidget(parent), client_(client), input_(input), contacts_(contacts), rooms_(rooms), avatars_(avatars)
{
    setObjectName(QStringLiteral("sidebarSearchPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8,4,8,8);
    directory_ = new QListWidget(this);
    directory_->setObjectName(QStringLiteral("searchDirectory"));
    directory_->setIconSize(QSize(32,32));
    layout->addWidget(directory_);
    layout->addWidget(new QLabel(QStringLiteral("聊天记录"),this));
    hits_ = new QListWidget(this);
    hits_->setObjectName(QStringLiteral("sidebarMessageHits"));
    hits_->setWordWrap(true);
    layout->addWidget(hits_,2);
    more_ = new QPushButton(QStringLiteral("更多聊天记录"),this);
    layout->addWidget(more_);
    more_->hide();
    auto* advanced = new QPushButton(QStringLiteral("高级搜索"),this);
    layout->addWidget(advanced);
    connect(advanced,&QPushButton::clicked,this,&sidebar_search::advanced_requested);
    connect(more_,&QPushButton::clicked,this,[this]{ if (next_ && !request_) search(next_); });
    debounce_.setSingleShot(true);
    debounce_.setInterval(220);
    connect(&debounce_,&QTimer::timeout,this,[this]{ search(0); });
    connect(&input_, &QLineEdit::textChanged, this,[this]{ refresh_directory(); restart(); });
    connect(&contacts_, &QAbstractItemModel::modelReset,this,&sidebar_search::refresh_directory);
    connect(&rooms_, &QAbstractItemModel::modelReset,this,&sidebar_search::refresh_directory);
    connect(&avatars_,&avatar_cache::changed,this,&sidebar_search::refresh_directory);
    auto activate = [this](QListWidgetItem* row)
    {
        auto id=row->data(Qt::UserRole).toLongLong();
        if (!id) return;
        if (id<0) emit room_requested(-id);
        else emit contact_requested(id,row->data(Qt::UserRole+1).toString());
    };
    connect(directory_, &QListWidget::itemClicked,this,activate);
    connect(directory_, &QListWidget::itemActivated,this,activate);
    auto locate = [this](QListWidgetItem* row)
    {
        auto const message=row->data(Qt::UserRole+1).toLongLong();
        if (message) emit message_requested(row->data(Qt::UserRole).toLongLong(),message);
    };
    connect(hits_,&QListWidget::itemClicked,this,locate);
    connect(hits_,&QListWidget::itemActivated,this,locate);
    connect(&client_,&client_bridge::global_search_received,this,[this](quint64 request,chat::message_search_page const& page,QString const& error)
    {
        if (request!=request_) return;
        request_=0;
        if (!error.isEmpty() || (page.messages.empty() && !hits_->count()))
        {
            auto* row=new QListWidgetItem(error.isEmpty()?QStringLiteral("没有找到相关消息"):error,hits_);
            row->setFlags(Qt::NoItemFlags);
        }
        for (auto const& hit:page.messages)
        {
            auto const snippet=QString::fromStdString(hit.snippet).simplified();
            auto const details=QStringLiteral("%1 · %2 · %3").arg(QString::fromStdString(hit.title),QString::fromStdString(hit.username),
                                  QDateTime::fromMSecsSinceEpoch(hit.timestamp).toString(QStringLiteral("MM-dd HH:mm")));
            auto* row=new QListWidgetItem(snippet + u'\n' + details,hits_);
            row->setToolTip(row->text());
            row->setData(Qt::UserRole,qint64(hit.conversation));
            row->setData(Qt::UserRole+1,qint64(hit.id));
        }
        if (error.isEmpty()) next_=page.next_before.value_or(0);
        more_->setVisible(next_>0);
    });
    // Drop the pending page before a new request can race with a withdrawal or loss of visibility.
    connect(&client_,&client_bridge::message_updated,this,[this](qint64, message_data const& msg, QString const& error){
        if (!error.isEmpty()) return;
        if (request_) { restart(); return; }
        for (int i=0;i<hits_->count();++i)
            if (hits_->item(i)->data(Qt::UserRole+1).toLongLong()==msg.id) { restart(); return; }
    });
    connect(&client_,&client_bridge::personal_view_changed,this,[this](qint64,QString const& action,qint64){if(action=="clear") restart();});
    connect(&client_,&client_bridge::conversation_changed,this,[this](qint64,bool removed){if(removed) restart();});
    connect(&client_,&client_bridge::disconnected,&input_,&QLineEdit::clear);
}
void sidebar_search::refresh_directory()
{
    directory_->clear();
    auto query=input_.text().trimmed();
    if(query.isEmpty()) return;
    auto heading=[this](QString const& text) { auto* item=new QListWidgetItem(text,directory_);item->setFlags(Qt::NoItemFlags); };
    bool first=true;
    for(int row=0;row<contacts_.rowCount();++row)
    {
        auto index=contacts_.index(row,0);
        if (!index.data(user_model::search_role).toString().contains(query,Qt::CaseInsensitive)) continue;
        if (first){heading(QStringLiteral("联系人"));first=false;}
        auto const& user=*contacts_.user_at(index);
        auto* item=new QListWidgetItem(avatar_icon(user.display_name(),32,avatars_.image(user.id)),user.display_name(),directory_);
        item->setData(Qt::UserRole,user.id);
        item->setData(Qt::UserRole+1,user.username);
    }
    first=true;
    for(auto const& room:rooms_.items())
    {
        if(!room.group || !room.username.contains(query,Qt::CaseInsensitive)) continue;
        if(first){heading(QStringLiteral("群聊"));first=false;}
        auto* item=new QListWidgetItem(avatar_icon(room.username,32,avatars_.image(room.avatar_key())),room.username,directory_);
        item->setData(Qt::UserRole,-room.id);
    }
    directory_->setVisible(directory_->count()>0);
}
void sidebar_search::restart()
{
    request_=next_=0;
    debounce_.stop();
    hits_->clear();
    more_->hide();
    if(!input_.text().isEmpty()) debounce_.start();
}
void sidebar_search::search(qint64 before)
{
    chat::message_search_filter filter;
    filter.query=input_.text().toStdString();
    request_=client_.search_all_messages(filter,before);
    more_->hide();
}
