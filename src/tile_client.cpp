/* Copyright 2018-2019 TomTom N.V.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include <chrono>
#include <QImage>
#include <QImageReader>
#include <QStandardPaths>
#include <QString>
#include <QtCore>
#include <QtNetwork>
#include <utility>
#include <regex>

#include "rcpputils/asserts.hpp"
#include "rviz_common/logging.hpp"
#include "tile_client.hpp"

namespace rviz_satellite
{

TileClient::TileClient()
: manager_(new QNetworkAccessManager(this)), tile_promises_()
{
  connect(manager_, SIGNAL(finished(QNetworkReply*)), SLOT(request_finished(QNetworkReply*)));
  QNetworkDiskCache * disk_cache = new QNetworkDiskCache(this);
  QString const cache_path =
    QDir(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)).filePath(
    "rviz_satellite");
  disk_cache->setCacheDirectory(cache_path);
  manager_->setCache(disk_cache);
}

/**
 * @brief Request a specific tile
 *
 * Since QNetworkDiskCache is used, tiles will be loaded from the file system if they have been cached.
 * Otherwise they are fetched from the tile server given in the @p tile_id.
 */
std::shared_future<QImage> TileClient::request(TileId const & tile_id)
{
  if (tile_id.server_url.find("file://") != std::string::npos) {
    return std::shared_future<QImage>(request_local(tile_id));
  } else {
    return request_remote(tile_id);
  }
}

std::shared_future<QImage> TileClient::request_remote(TileId const & tile_id)
{
  // see https://foundation.wikimedia.org/wiki/Maps_Terms_of_Use#Using_maps_in_third-party_services
  auto const request_url = QUrl(QString::fromStdString(tileURL(tile_id)));
  QNetworkRequest request(request_url);
  char constexpr agent[] =
    "rviz_satellite " RVIZ_SATELLITE_VERSION " (https://github.com/Kettenhoax/rviz_satellite)";
  request.setHeader(QNetworkRequest::KnownHeaders::UserAgentHeader, agent);
  QVariant variant;
  variant.setValue(tile_id);
  request.setAttribute(
    QNetworkRequest::CacheLoadControlAttribute,
    QNetworkRequest::CacheLoadControl::PreferCache);
  request.setAttribute(QNetworkRequest::User, variant);

  // Check if this tile request already exists
  auto future_it = tile_futures_.find(tile_id);
  if (future_it != tile_futures_.end()) {
    auto time_it = tile_request_times_.find(tile_id);
    auto elapsed = std::chrono::steady_clock::now() - time_it->second;
    
    // If request is still within timeout window, return existing shared future (deduplication)
    if (elapsed < REQUEST_TIMEOUT) {
      RVIZ_COMMON_LOG_DEBUG_STREAM(
        "Tile request for tile '" << tile_id << "' already in progress, returning existing future");
      return future_it->second;
    } else {
      // Request has timed out, clean up and retry
      RVIZ_COMMON_LOG_WARNING_STREAM(
        "Tile request for tile '" << tile_id << "' timed out after " <<
        REQUEST_TIMEOUT.count() << "s, retrying...");
      tile_futures_.erase(future_it);
      tile_promises_.erase(tile_id);
      tile_request_times_.erase(time_it);
    }
  }

  std::promise<QImage> tile_promise;
  auto future = tile_promise.get_future();
  auto shared_future = std::shared_future<QImage>(std::move(future));
  tile_futures_[tile_id] = shared_future;
  auto promise_entry = tile_promises_.emplace(tile_id, std::move(tile_promise));
  rcpputils::assert_true(promise_entry.second, "Failed to insert new tile promise");
  
  tile_request_times_[tile_id] = std::chrono::steady_clock::now();
  RVIZ_COMMON_LOG_DEBUG_STREAM("Requesting tile " << request_url.toString().toStdString());
  manager_->get(request);
  
  return shared_future;
}

std::future<QImage> TileClient::request_local(TileId const & tile_id)
{
  std::future<QImage> f = std::async(std::launch::async, [tile_id]{
    auto const filename_uri = tileURL(tile_id);

    auto filename = std::regex_replace(filename_uri, std::regex("file://"), "");

    QImageReader reader(QString::fromStdString(filename));

    if (!reader.canRead())
    {
      RVIZ_COMMON_LOG_DEBUG_STREAM("Unable to decode image at " << filename);
      return QImage{ };
    }

    auto image = reader.read().mirrored();

    if (image.isNull())
    {
      RVIZ_COMMON_LOG_DEBUG_STREAM("QImageReader able to decode but read failed for " << filename);
    }

    return image;
  });

  return f;
}

void TileClient::request_finished(QNetworkReply * reply)
{
  const QVariant variant = reply->request().attribute(QNetworkRequest::User);
  auto tile_id = variant.value<TileId>();

  auto promise_it = tile_promises_.find(tile_id);
  // Erase the element pointed by iterator it
  if (promise_it == tile_promises_.end()) {
    RVIZ_COMMON_LOG_ERROR_STREAM(
      "Tile request promise was removed before the network reply finished");
    reply->deleteLater();
    return;
  }

  // Clean up request timestamp and cached future
  auto time_it = tile_request_times_.find(tile_id);
  if (time_it != tile_request_times_.end()) {
    tile_request_times_.erase(time_it);
  }

  const QUrl url = reply->url();
  if (reply->error()) {
    RVIZ_COMMON_LOG_ERROR_STREAM(
      "Tile request failed: " << reply->errorString().toStdString() << " for " <<
      url.toString().toStdString());
    promise_it->second.set_exception(
      std::make_exception_ptr(
        tile_request_error(reply->errorString().toStdString())));
    tile_promises_.erase(promise_it);
    tile_futures_.erase(tile_id);
    reply->deleteLater();
    return;
  }

  // log if tile comes from cache or web
  bool const from_cache = reply->attribute(QNetworkRequest::SourceIsFromCacheAttribute).toBool();
  if (from_cache) {
    RVIZ_COMMON_LOG_DEBUG_STREAM("Loaded tile from cache " << url.toString().toStdString());
  } else {
    RVIZ_COMMON_LOG_DEBUG_STREAM("Loaded tile from web " << url.toString().toStdString());
  }

  QImageReader reader(reply);
  if (!reader.canRead()) {
    promise_it->second.set_exception(
      std::make_exception_ptr(
        tile_request_error(
          "Failed to decode tile image")));
    RVIZ_COMMON_LOG_ERROR_STREAM(
      "Failed to decode image at " << reply->request().url().toString().toStdString());
    tile_promises_.erase(promise_it);
    tile_futures_.erase(tile_id);
    reply->deleteLater();
    return;
  }
  promise_it->second.set_value(reader.read().mirrored());
  tile_promises_.erase(promise_it);
  tile_futures_.erase(tile_id);
  reply->deleteLater();
}

}  // namespace rviz_satellite
