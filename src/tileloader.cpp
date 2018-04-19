/*
 * TileLoader.cpp
 *
 *  Copyright (c) 2014 Gaeth Cross. Apache 2 License.
 *
 *  This file is part of rviz_satellite.
 *
 *	Created on: 07/09/2014
 */

#include "tileloader.h"

#include <QUrl>
#include <QNetworkRequest>
#include <QNetworkProxy>
#include <QVariant>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <stdexcept>
#include <boost/regex.hpp>
#include <ros/ros.h>
#include <ros/package.h>
#include <functional> // for std::hash


static size_t replaceRegex(const boost::regex &ex, std::string &str,
                           const std::string &replace) {
  std::string::const_iterator start = str.begin(), end = str.end();
  boost::match_results<std::string::const_iterator> what;
  boost::match_flag_type flags = boost::match_default;
  size_t count = 0;
  while (boost::regex_search(start, end, what, ex, flags)) {
    str.replace(what.position(), what.length(), replace);
    start = what[0].second;
    count++;
  }
  return count;
}

void TileLoader::MapTile::abortLoading() {
  if (reply_) {
    reply_->abort();
    reply_ = nullptr;
  }
}

bool TileLoader::MapTile::hasImage() const { return !image_.isNull(); }

TileLoader::TileLoader(const std::string &service, double latitude,
                       double longitude, unsigned int zoom, unsigned int blocks,
                       const std::string &proxy,  const std::string &cache_base_path,
                       bool offline_mode,
                       QObject *parent)
    : QObject(parent), latitude_(latitude), longitude_(longitude), zoom_(zoom),
      blocks_(blocks),  object_uri_(service), proxy_(proxy),
      cache_path_(),  offline_mode_(offline_mode) {
  assert(blocks_ >= 0);

  std::hash<std::string> hash_fn;
  cache_path_ =
      QDir::cleanPath(QString::fromStdString(cache_base_path) + QDir::separator() +
                      QString::number(hash_fn(object_uri_)));

  QDir dir(cache_path_);
  if (!dir.exists() && !dir.mkpath(".")) {
    throw std::runtime_error("Failed to create cache folder: " +
                             cache_path_.toStdString());
  }



  // Override proxy if specified
  QString proxy_uri = QString::fromStdString(proxy_);
  QStringList kStr = proxy_uri.split(':');

  if(kStr.size() == 2) {
    _localhostProxy = QNetworkProxy(QNetworkProxy::HttpProxy, kStr[0], kStr[1].toUInt());

    QString hostname = _localhostProxy.hostName();
    QString port =  QString::number(_localhostProxy.port());
    ROS_DEBUG("Proxy initialized to %s:%s",hostname.toStdString().c_str(), port.toStdString().c_str());

  } else {
    _localhostProxy = QNetworkProxy(QNetworkProxy::HttpProxy, QString(), 0);
  }

  /// @todo: some kind of error checking of the URL

  //  calculate center tile coordinates
  double x, y;
  latLonToTileCoords(latitude_, longitude_, zoom_, x, y);
  center_tile_x_ = std::floor(x);
  center_tile_y_ = std::floor(y);
  //  fractional component
  origin_offset_x_ = x - center_tile_x_;
  origin_offset_y_ = y - center_tile_y_;
}

bool TileLoader::insideCentreTile(double lat, double lon) const {
  double x, y;
  latLonToTileCoords(lat, lon, zoom_, x, y);
  return (std::floor(x) == center_tile_x_ && std::floor(y) == center_tile_y_);
}

void TileLoader::start() {
  //  discard previous set of tiles and all pending requests
  abort();

  ROS_DEBUG("loading %d blocks around tile=(%d,%d)", blocks_, center_tile_x_, center_tile_y_ );

  qnam_.reset( new QNetworkAccessManager(this) );
  QObject::connect(qnam_.get(), SIGNAL(finished(QNetworkReply *)), this,
                   SLOT(finishedRequest(QNetworkReply *)));


  if(!_localhostProxy.hostName().isEmpty()) {
    qnam_->proxyFactory()->setUseSystemConfiguration ( false );
    qnam_->setProxy(_localhostProxy);

    QString hostname = _localhostProxy.hostName();
    QString port =  QString::number(_localhostProxy.port());
    ROS_DEBUG("Proxy updated to %s:%s",hostname.toStdString().c_str(), port.toStdString().c_str());

  } else {
    qnam_->proxyFactory()->setUseSystemConfiguration ( true );
  }

  //  determine what range of tiles we can load
  const int min_x = std::max(0, center_tile_x_ - blocks_);
  const int min_y = std::max(0, center_tile_y_ - blocks_);
  const int max_x = std::min(maxTiles(), center_tile_x_ + blocks_);
  const int max_y = std::min(maxTiles(), center_tile_y_ + blocks_);

  //  initiate requests
  for (int y = min_y; y <= max_y; y++) {
    for (int x = min_x; x <= max_x; x++) {
      // Generate filename
      const QString full_path = cachedPathForTile(x, y, zoom_);

      // Check if tile is already in the cache
      QFile tile(full_path);
      if (tile.exists()) {
        QImage image(full_path);
        tiles_.push_back(MapTile(x, y, zoom_, image));
      } else {

        if(!offline_mode_) {
          const QUrl uri = uriForTile(x, y);
          //  send request
          QNetworkRequest request = QNetworkRequest(uri);
          auto const userAgent = QByteArray("rviz_satellite/0.0.2 (+https://github.com/gareth-cross/rviz_satellite)");
          request.setRawHeader(QByteArray("User-Agent"), userAgent);
          QNetworkReply *rep = qnam_->get(request);
          emit initiatedRequest(request);
          tiles_.push_back(MapTile(x, y, zoom_, rep));
        }
      }
    }
  }

  checkIfLoadingComplete();
}

double TileLoader::resolution() const {
  return zoomToResolution(latitude_, zoom_);
}

/// @see http://wiki.openstreetmap.org/wiki/Slippy_map_tilenames
/// For explanation of these calculations.
void TileLoader::latLonToTileCoords(double lat, double lon, unsigned int zoom,
                                    double &x, double &y) {
  if (zoom > 31) {
    throw std::invalid_argument("Zoom level " + std::to_string(zoom) +
                                " too high");
  } else if (lat < -85.0511 || lat > 85.0511) {
    throw std::invalid_argument("Latitude " + std::to_string(lat) + " invalid");
  } else if (lon < -180 || lon > 180) {
    throw std::invalid_argument("Longitude " + std::to_string(lon) +
                                " invalid");
  }

  const double rho = M_PI / 180;
  const double lat_rad = lat * rho;

  unsigned int n = (1 << zoom);
  x = n * ((lon + 180) / 360.0);
  y = n * (1 - (std::log(std::tan(lat_rad) + 1 / std::cos(lat_rad)) / M_PI)) /
      2;
  ROS_DEBUG_STREAM( "Center tile coords: " << x << ", " << y );
}

double TileLoader::zoomToResolution(double lat, unsigned int zoom) {
  const double lat_rad = lat * M_PI / 180;
  return 156543.034 * std::cos(lat_rad) / (1 << zoom);
}

void TileLoader::finishedRequest(QNetworkReply *reply) {
  const QNetworkRequest request = reply->request();


  QVariant possibleRedirectUrl = reply->attribute(QNetworkRequest::RedirectionTargetAttribute);

   /* We'll deduct if the redirection is valid in the redirectUrl function */
   _urlRedirectedTo = this->redirectUrl(possibleRedirectUrl.toUrl(),
                                        _urlRedirectedTo);

   /* If the URL is not empty, we're being redirected. */
   if(!_urlRedirectedTo.isEmpty()) {
       QString text = QString("QNAMRedirect::replyFinished: Redirected to ")
                             .append(_urlRedirectedTo.toString());
       emit warnOcurred(text);
       /* We'll do another request to the redirection url. */
       qnam_->get(QNetworkRequest(_urlRedirectedTo));
   } else {
      //  find corresponding tile
      const std::vector<MapTile>::iterator it =
          std::find_if(tiles_.begin(), tiles_.end(),
                       [&](const MapTile &tile) { return tile.reply() == reply; });
      if (it == tiles_.end()) {
        //  removed from list already, ignore this reply
        return;
      }
      MapTile &tile = *it;

      if (reply->error() == QNetworkReply::NoError) {
        //  decode an image
        QImageReader reader(reply);
        if (reader.canRead()) {
          QImage image = reader.read();
          tile.setImage(image);
          image.save(cachedPathForTile(tile.x(), tile.y(), tile.z()), "JPEG");
          emit receivedImage(request);
        } else {
          //  probably not an image
          QString err;
          err = "Unable to decode image at " + request.url().toString();
          emit errorOcurred(err);
        }
      } else {
        const QString err = "Failed loading " + request.url().toString() +
                            " with code " + QString::number(reply->error());
        emit errorOcurred(err);
      }
      checkIfLoadingComplete();
   }
   /* Clean up. */
   reply->deleteLater();
}


QUrl TileLoader::redirectUrl(const QUrl& possibleRedirectUrl,
                               const QUrl& oldRedirectUrl) const {
    QUrl redirectUrl;
    /*
     * Check if the URL is empty and
     * that we aren't being fooled into a infinite redirect loop.
     * We could also keep track of how many redirects we have been to
     * and set a limit to it, but we'll leave that to you.
     */
    if(!possibleRedirectUrl.isEmpty() &&
       possibleRedirectUrl != oldRedirectUrl) {
        redirectUrl = possibleRedirectUrl;
    }
    return redirectUrl;
}

bool TileLoader::checkIfLoadingComplete() {
  const bool loaded =
      std::all_of(tiles_.begin(), tiles_.end(),
                  [](const MapTile &tile) { return tile.hasImage(); });
  if (loaded) {
    emit finishedLoading();
  }
  return loaded;
}

QUrl TileLoader::uriForTile(int x, int y) const {
  std::string object = object_uri_;
  //  place {x},{y},{z} with appropriate values
  replaceRegex(boost::regex("\\{x\\}", boost::regex::icase), object,
               std::to_string(x));
  replaceRegex(boost::regex("\\{y\\}", boost::regex::icase), object,
               std::to_string(y));
  replaceRegex(boost::regex("\\{z\\}", boost::regex::icase), object,
               std::to_string(zoom_));

  const QString qstr = QString::fromStdString(object);
  return QUrl(qstr);
}

QString TileLoader::cachedNameForTile(int x, int y, int z) const {
  return "x" + QString::number(x) + "_y" + QString::number(y) + "_z" +
         QString::number(z) + ".jpg";
}

QString TileLoader::cachedPathForTile(int x, int y, int z) const {
  return QDir::cleanPath(cache_path_ + QDir::separator() +
                         cachedNameForTile(x, y, z));
}

int TileLoader::maxTiles() const { return (1 << zoom_) - 1; }

void TileLoader::abort() {
  tiles_.clear();
  //  destroy network access manager
  qnam_.reset();
}
