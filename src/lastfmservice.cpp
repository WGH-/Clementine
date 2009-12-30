#include "lastfmservice.h"
#include "lastfmconfig.h"
#include "radioitem.h"
#include "song.h"

#include <lastfm/ws.h>
#include <lastfm/misc.h>
#include <lastfm/XmlQuery>
#include <lastfm/Audioscrobbler>

#include <QSettings>
#include <QMenu>

const char* LastFMService::kServiceName = "Last.fm";
const char* LastFMService::kSettingsGroup = "Last.fm";
const char* LastFMService::kAudioscrobblerClientId = "tng";
const char* LastFMService::kApiKey = "75d20fb472be99275392aefa2760ea09";
const char* LastFMService::kSecret = "d3072b60ae626be12be69448f5c46e70";

LastFMService::LastFMService(QObject* parent)
  : RadioService(kServiceName, parent),
    tuner_(NULL),
    scrobbler_(NULL),
    context_menu_(new QMenu),
    initial_tune_(false),
    scrobbling_enabled_(false),
    friends_list_(NULL),
    neighbours_list_(NULL)
{
  lastfm::ws::ApiKey = kApiKey;
  lastfm::ws::SharedSecret = kSecret;

  QSettings settings;
  settings.beginGroup(kSettingsGroup);
  lastfm::ws::Username = settings.value("username").toString();
  lastfm::ws::SessionKey = settings.value("session").toString();
  scrobbling_enabled_ = settings.value("scrobbling_enabled", true).toBool();

  config_ = new LastFMConfig(this);
  connect(config_, SIGNAL(ScrobblingEnabledChanged(bool)), SLOT(ScrobblingEnabledChangedSlot(bool)));

  config_->ui_.username->setText(lastfm::ws::Username);
  config_->ui_.scrobble->setEnabled(scrobbling_enabled_);

  play_action_ = context_menu_->addAction(QIcon(":media-playback-start.png"), "Add to playlist", this, SLOT(AddToPlaylist()));
  context_menu_->addSeparator();
  context_menu_->addAction(QIcon(":configure.png"), "Configure Last.fm...",
                           config_, SLOT(show()));

}

LastFMService::~LastFMService() {
  delete config_;
  delete context_menu_;
}

bool LastFMService::IsAuthenticated() const {
  return !lastfm::ws::SessionKey.isEmpty();
}

void LastFMService::ScrobblingEnabledChangedSlot(bool value) {
  scrobbling_enabled_ = value;

  QSettings settings;
  settings.beginGroup(kSettingsGroup);
  settings.setValue("scrobbling_enabled", scrobbling_enabled_);

  emit ScrobblingEnabledChanged(value);
}

RadioItem* LastFMService::CreateRootItem(RadioItem* parent) {
  RadioItem* item = new RadioItem(this, RadioItem::Type_Service, "Last.fm", parent);
  item->icon = QIcon(":last.fm/as.png");
  return item;
}

void LastFMService::LazyPopulate(RadioItem *item) {
  RadioItem* c = NULL;

  switch (item->type) {
    case RadioItem::Type_Service:
      // Normal radio types
      CreateStationItem(Type_MyRecommendations, "My Recommendations", ":last.fm/recommended_radio.png", item);
      CreateStationItem(Type_MyRadio, "My Radio Station", ":last.fm/personal_radio.png", item);
      CreateStationItem(Type_MyLoved, "My Loved Tracks", ":last.fm/loved_radio.png", item);
      CreateStationItem(Type_MyNeighbourhood, "My Neighbourhood", ":last.fm/neighbour_radio.png", item);

      // Types that spawn a popup dialog
      c = CreateStationItem(Type_ArtistRadio, "Artist radio...",
                            ":last.fm/icon_radio.png", item);
      c->playable = false;

      c = CreateStationItem(Type_TagRadio, "Tag radio...",
                            ":last.fm/icon_tag.png", item);
      c->playable = false;

      // Types that have children
      friends_list_ = new RadioItem(this, Type_MyFriends, "Friends", item);
      friends_list_->icon = QIcon(":last.fm/my_friends.png");

      neighbours_list_ = new RadioItem(this, Type_MyNeighbours, "Neighbours", item);
      neighbours_list_->icon = QIcon(":last.fm/my_neighbours.png");

      if (!IsAuthenticated())
        config_->show();
      break;

    case Type_MyFriends:
      RefreshFriends();
      break;

    case Type_MyNeighbours:
      RefreshNeighbours();
      break;

    case Type_OtherUser:
      CreateStationItem(Type_OtherUserRadio, item->key, ":last.fm/recommended_radio.png", item)
          ->display_text = item->key + "'s Radio Station";
      CreateStationItem(Type_OtherUserLoved, item->key, ":last.fm/loved_radio.png", item)
          ->display_text = item->key + "'s Loved Tracks";
      CreateStationItem(Type_OtherUserNeighbourhood, item->key, ":last.fm/neighbour_radio.png", item)
          ->display_text = item->key + "'s Neighbourhood";
      break;

    default:
      break;
  }

  item->lazy_loaded = true;
}

RadioItem* LastFMService::CreateStationItem(ItemType type, const QString& name,
                                            const QString& icon, RadioItem* parent) {
  RadioItem* ret = new RadioItem(this, type, name, parent);
  ret->lazy_loaded = true;
  ret->icon = QIcon(icon);
  ret->playable = true;

  return ret;
}

void LastFMService::Authenticate(const QString& username, const QString& password) {
  QMap<QString, QString> params;
  params["method"] = "auth.getMobileSession";
  params["username"] = username;
  params["authToken"] = lastfm::md5((username + lastfm::md5(password.toUtf8())).toUtf8());

  QNetworkReply* reply = lastfm::ws::post(params);
  connect(reply, SIGNAL(finished()), SLOT(AuthenticateReplyFinished()));
}

void LastFMService::AuthenticateReplyFinished() {
  QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
  if (!reply) {
    emit AuthenticationComplete(false);
    return;
  }

  // Parse the reply
  try {
    lastfm::XmlQuery const lfm = lastfm::ws::parse(reply);

    lastfm::ws::Username = lfm["session"]["name"].text();
    lastfm::ws::SessionKey = lfm["session"]["key"].text();
  } catch (std::runtime_error& e) {
    qDebug() << e.what();
    emit AuthenticationComplete(false);
    return;
  }

  // Save the session key
  QSettings settings;
  settings.beginGroup(kSettingsGroup);
  settings.setValue("username", lastfm::ws::Username);
  settings.setValue("session", lastfm::ws::SessionKey);

  // Invalidate the scrobbler - it will get recreated later
  delete scrobbler_;
  scrobbler_ = NULL;

  emit AuthenticationComplete(true);
}

QUrl LastFMService::UrlForItem(const RadioItem* item) const {
  switch (item->type) {
    case Type_MyRecommendations:
      return "lastfm://user/" + lastfm::ws::Username + "/recommended";

    case Type_MyLoved:
      return "lastfm://user/" + lastfm::ws::Username + "/loved";

    case Type_MyNeighbourhood:
      return "lastfm://user/" + lastfm::ws::Username + "/neighbours";

    case Type_MyRadio:
      return "lastfm://user/" + lastfm::ws::Username + "/library";

    case Type_OtherUser:
    case Type_OtherUserRadio:
      return "lastfm://user/" + item->key + "/library";

    case Type_OtherUserLoved:
      return "lastfm://user/" + item->key + "/loved";

    case Type_OtherUserNeighbourhood:
      return "lastfm://user/" + item->key + "/neighbours";
  }
  return QUrl();
}

QString LastFMService::TitleForItem(const RadioItem* item) const {
  const QString me(lastfm::ws::Username);

  switch (item->type) {
    case Type_MyRecommendations: return me + "'s Recommended Radio";
    case Type_MyLoved:           return me + "'s Loved Tracks";
    case Type_MyNeighbourhood:   return me + "'s Neighbour Radio";
    case Type_MyRadio:           return me + "'s Library";
    case Type_OtherUser:
    case Type_OtherUserRadio:    return item->key + "'s Library";
    case Type_OtherUserLoved:    return item->key + "'s Loved Tracks";
    case Type_OtherUserNeighbourhood: return item->key + "'s Neighbour Radio";
  }
  return QString();
}

void LastFMService::StartLoading(const QUrl& url) {
  if (url.scheme() != "lastfm")
    return;
  if (!IsAuthenticated())
    return;

  emit LoadingStarted();

  delete tuner_;

  last_url_ = url;
  initial_tune_ = true;
  tuner_ = new lastfm::RadioTuner(lastfm::RadioStation(url));

  connect(tuner_, SIGNAL(trackAvailable()), SLOT(TunerTrackAvailable()));
  connect(tuner_, SIGNAL(error(lastfm::ws::Error)), SLOT(TunerError(lastfm::ws::Error)));
}

void LastFMService::LoadNext(const QUrl &) {
  last_track_ = tuner_->takeNextTrack();

  if (last_track_.isNull()) {
    emit StreamFinished();
    return;
  }

  Song metadata;
  metadata.InitFromLastFM(last_track_);

  emit StreamMetadataFound(last_url_, metadata);
  emit StreamReady(last_url_, last_track_.url());
}

void LastFMService::TunerError(lastfm::ws::Error error) {
  qDebug() << "Last.fm error" << error;
  if (!initial_tune_)
    return;

  emit LoadingFinished();

  if (error == lastfm::ws::NotEnoughContent) {
    emit StreamFinished();
    return;
  }

  emit StreamError(ErrorString(error));
}

QString LastFMService::ErrorString(lastfm::ws::Error error) const {
  switch (error) {
    case lastfm::ws::InvalidService: return "Invalid service";
    case lastfm::ws::InvalidMethod: return "Invalid method";
    case lastfm::ws::AuthenticationFailed: return "Authentication failed";
    case lastfm::ws::InvalidFormat: return "Invalid format";
    case lastfm::ws::InvalidParameters: return "Invalid parameters";
    case lastfm::ws::InvalidResourceSpecified: return "Invalid resource specified";
    case lastfm::ws::OperationFailed: return "Operation failed";
    case lastfm::ws::InvalidSessionKey: return "Invalid session key";
    case lastfm::ws::InvalidApiKey: return "Invalid API key";
    case lastfm::ws::ServiceOffline: return "Service offline";
    case lastfm::ws::SubscribersOnly: return "This stream is for paid subscribers only";

    case lastfm::ws::TryAgainLater: return "Last.fm is currently busy, please try again in a few minutes";

    case lastfm::ws::NotEnoughContent: return "Not enough content";
    case lastfm::ws::NotEnoughMembers: return "Not enough members";
    case lastfm::ws::NotEnoughFans: return "Not enough fans";
    case lastfm::ws::NotEnoughNeighbours: return "Not enough neighbours";

    case lastfm::ws::MalformedResponse: return "Malformed response";

    case lastfm::ws::UnknownError:
    default:
      return "Unknown error";
  }
}

void LastFMService::TunerTrackAvailable() {
  if (initial_tune_) {
    emit LoadingFinished();

    LoadNext(last_url_);
    initial_tune_ = false;
  }
}

bool LastFMService::InitScrobbler() {
  if (!IsAuthenticated() || !IsScrobblingEnabled())
    return false;

  if (!scrobbler_)
    scrobbler_ = new lastfm::Audioscrobbler(kAudioscrobblerClientId);

  return true;
}

lastfm::Track LastFMService::TrackFromSong(const Song &song) const {
  if (song.title() == last_track_.title() &&
      song.artist() == last_track_.artist() &&
      song.album() == last_track_.album())
    return last_track_;

  lastfm::Track ret;
  song.ToLastFM(&ret);
  return ret;

}

void LastFMService::NowPlaying(const Song &song) {
  if (!InitScrobbler())
    return;

  last_track_ = TrackFromSong(song);

  lastfm::MutableTrack mtrack(last_track_);
  mtrack.stamp();

  scrobbler_->nowPlaying(last_track_);
}

void LastFMService::Scrobble() {
  if (!InitScrobbler())
    return;

  scrobbler_->cache(last_track_);
  scrobbler_->submit();
}

void LastFMService::Love() {
  if (!IsAuthenticated())
    config_->show();

  lastfm::MutableTrack mtrack(last_track_);
  mtrack.love();
}

void LastFMService::Ban() {
  lastfm::MutableTrack mtrack(last_track_);
  mtrack.ban();

  Scrobble();
  LoadNext(last_url_);
}

void LastFMService::ShowContextMenu(RadioItem* item, const QPoint &global_pos) {
  context_item_ = item;

  play_action_->setEnabled(item->playable);
  context_menu_->popup(global_pos);
}

void LastFMService::RefreshFriends() {
  if (!friends_list_ || !IsAuthenticated())
    return;

  friends_list_->ClearNotify();

  lastfm::AuthenticatedUser user;
  QNetworkReply* reply = user.getFriends();
  connect(reply, SIGNAL(finished()), SLOT(RefreshFriendsFinished()));
}

void LastFMService::RefreshNeighbours() {
  if (!friends_list_ || !IsAuthenticated())
    return;

  neighbours_list_->ClearNotify();

  lastfm::AuthenticatedUser user;
  QNetworkReply* reply = user.getNeighbours();
  connect(reply, SIGNAL(finished()), SLOT(RefreshNeighboursFinished()));
}

void LastFMService::RefreshFriendsFinished() {
  QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
  if (!reply)
    return;

  QList<lastfm::User> friends;

  try {
    friends = lastfm::User::list(reply);
  } catch (std::runtime_error& e) {
    qDebug() << e.what();
    return;
  }

  foreach (const lastfm::User& f, friends) {
    RadioItem* item = new RadioItem(this, Type_OtherUser, f);
    item->icon = QIcon(":last.fm/icon_user.png");
    item->playable = true;
    item->InsertNotify(friends_list_);
  }
}

void LastFMService::RefreshNeighboursFinished() {
  QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
  if (!reply)
    return;

  QList<lastfm::User> neighbours;

  try {
    neighbours = lastfm::User::list(reply);
  } catch (std::runtime_error& e) {
    qDebug() << e.what();
    return;
  }

  foreach (const lastfm::User& n, neighbours) {
    RadioItem* item = new RadioItem(this, Type_OtherUser, n);
    item->icon = QIcon(":last.fm/user_purple.png");
    item->playable = true;
    item->InsertNotify(neighbours_list_);
  }
}

void LastFMService::AddToPlaylist() {
  emit AddItemToPlaylist(context_item_);
}
