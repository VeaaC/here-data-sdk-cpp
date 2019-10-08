/*
 * Copyright (C) 2019 HERE Europe B.V.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 * License-Filename: LICENSE
 */

#include <gmock/gmock.h>
#include <matchers/NetworkUrlMatchers.h>
#include <mocks/CacheMock.h>
#include <mocks/NetworkMock.h>
#include <olp/core/client/OlpClientSettings.h>
#include <olp/dataservice/read/DataRequest.h>
#include <olp/dataservice/read/model/Partitions.h>
#include "../src/repositories/PartitionsRepository.h"

// clang-format off
#include "generated/parser/PartitionsParser.h"
#include <olp/core/generated/parser/JsonParser.h>
// clang-format on

namespace {
using namespace olp;
using namespace client;
using namespace dataservice::read;

#define OLP_SDK_URL_LOOKUP_METADATA \
  R"(https://api-lookup.data.api.platform.here.com/lookup/v1/resources/hrn:here:data:::hereos-internal-test-v2/apis/query/v1)"
#define OLP_SDK_HTTP_RESPONSE_LOOKUP_METADATA \
  R"jsonString([{"api":"metadata","version":"v1","baseURL":"https://metadata.data.api.platform.here.com/metadata/v1/catalogs/hereos-internal-test-v2","parameters":{}}])jsonString"

#define OLP_SDK_URL_PARTITION_BY_ID \
  R"(https://metadata.data.api.platform.here.com/metadata/v1/catalogs/hereos-internal-test-v2/layers/test_layer/partitions?partition=269&version=4)"
#define OLP_SDK_HTTP_RESPONSE_PARTITION_BY_ID \
  R"jsonString({ "partitions": [{"version":42,"partition":"1111","layer":"olp-cpp-sdk-ingestion-test-volatile-layer","dataHandle":"PartitionsRepositoryTest-partitionId"}]})jsonString"

const std::string kCacheKeyMetadata =
    "hrn:here:data:::hereos-internal-test-v2::query::v1::api";
const std::string kCacheKeyPartition =
    "hrn:here:data:::hereos-internal-test-v2::test_layer::1111::4::partition";

TEST(PartitionsRepositoryTest, GetPartitionById) {
  using namespace testing;
  using testing::Return;

  auto cache = std::make_shared<testing::StrictMock<CacheMock>>();
  auto network = std::make_shared<testing::StrictMock<NetworkMock>>();

  const std::string catalog = "hrn:here:data:::hereos-internal-test-v2";
  const std::string layer_id = "test_layer";
  const std::string partition_id = "269";
  const int version = 4;

  const auto catalog_hrn = HRN::FromString(catalog);

  OlpClientSettings settings;
  settings.cache = cache;
  settings.network_request_handler = network;
  settings.retry_settings.timeout = 1;

  const DataRequest request{
      DataRequest().WithPartitionId(partition_id).WithVersion(version)};

  const std::string cache_key = catalog + "::" + layer_id +
                                "::" + partition_id +
                                "::" + std::to_string(version) + "::partition";

  auto setup_online_only_mocks = [&]() {
    ON_CALL(*cache, Get(_, _))
        .WillByDefault([](const std::string&, const olp::cache::Decoder&) {
          ADD_FAILURE() << "Cache should not be used in OnlineOnly request";
          return boost::any{};
        });
  };

  auto setup_positive_metadata_mocks = [&]() {
    EXPECT_CALL(*network,
                Send(IsGetRequest(OLP_SDK_URL_LOOKUP_METADATA), _, _, _, _))
        .WillOnce(NetworkMock::ReturnHttpResponse(
            olp::http::NetworkResponse().WithStatus(
                olp::http::HttpStatusCode::OK),
            OLP_SDK_HTTP_RESPONSE_LOOKUP_METADATA));

    EXPECT_CALL(*cache, Put(Eq(kCacheKeyMetadata), _, _, _)).Times(1);
  };

  {
    SCOPED_TRACE("Fetch from cache [CacheOnly] positive");

    const std::string query_cache_response =
        R"jsonString({"version":4,"partition":"269","layer":"testlayer","dataHandle":"qwerty"})jsonString";

    EXPECT_CALL(*cache, Get(cache_key, _))
        .Times(1)
        .WillOnce(
            Return(parser::parse<model::Partition>(query_cache_response)));

    client::CancellationContext context;
    auto response = repository::PartitionsRepository::GetPartitionById(
        catalog_hrn, layer_id, context,
        DataRequest(request).WithFetchOption(CacheOnly), settings);

    ASSERT_TRUE(response.IsSuccessful());
    const auto& result = response.GetResult();
    const auto& partitions = result.GetPartitions();
    EXPECT_EQ(partitions.size(), 1);
    const auto& partition = partitions.front();
    EXPECT_EQ(partition.GetDataHandle(), "qwerty");
    EXPECT_EQ(partition.GetVersion().value_or(0), version);
    EXPECT_EQ(partition.GetPartition(), partition_id);

    Mock::VerifyAndClearExpectations(cache.get());
  }
  {
    SCOPED_TRACE("Fetch from cache [CacheOnly] negative");

    EXPECT_CALL(*cache, Get(cache_key, _))
        .Times(1)
        .WillOnce(Return(boost::any()));

    client::CancellationContext context;
    auto response = repository::PartitionsRepository::GetPartitionById(
        catalog_hrn, layer_id, context,
        DataRequest(request).WithFetchOption(CacheOnly), settings);

    ASSERT_FALSE(response.IsSuccessful());
    const auto& result = response.GetError();
    EXPECT_EQ(result.GetErrorCode(), ErrorCode::NotFound);

    Mock::VerifyAndClearExpectations(cache.get());
  }
  {
    SCOPED_TRACE("Fetch with missing partition id");

    client::CancellationContext context;
    auto response = repository::PartitionsRepository::GetPartitionById(
        catalog_hrn, layer_id, context,
        DataRequest(request).WithPartitionId(boost::none), settings);

    ASSERT_FALSE(response.IsSuccessful());
    const auto& result = response.GetError();
    EXPECT_EQ(result.GetErrorCode(), ErrorCode::PreconditionFailed);

    Mock::VerifyAndClearExpectations(cache.get());
  }
  {
    SCOPED_TRACE("Fetch with missing version");

    client::CancellationContext context;
    auto response = repository::PartitionsRepository::GetPartitionById(
        catalog_hrn, layer_id, context,
        DataRequest(request).WithVersion(boost::none), settings);

    ASSERT_FALSE(response.IsSuccessful());
    const auto& result = response.GetError();
    EXPECT_EQ(result.GetErrorCode(), ErrorCode::PreconditionFailed);

    Mock::VerifyAndClearExpectations(cache.get());
  }

  {
    SCOPED_TRACE("Fetch from network");
    setup_online_only_mocks();
    setup_positive_metadata_mocks();

    client::CancellationContext context;
    EXPECT_CALL(*network,
                Send(IsGetRequest(OLP_SDK_URL_PARTITION_BY_ID), _, _, _, _))
        .WillOnce(NetworkMock::ReturnHttpResponse(
            olp::http::NetworkResponse().WithStatus(
                olp::http::HttpStatusCode::OK),
            OLP_SDK_HTTP_RESPONSE_PARTITION_BY_ID));

    EXPECT_CALL(*cache, Put(Eq(kCacheKeyPartition), _, _, _)).Times(1);

    auto response = repository::PartitionsRepository::GetPartitionById(
        catalog_hrn, layer_id, context,
        DataRequest(request).WithFetchOption(OnlineOnly), settings);

    ASSERT_TRUE(response.IsSuccessful());
    const auto& partitions = response.GetResult().GetPartitions();
    EXPECT_EQ(partitions.size(), 1);
    const auto& partition = partitions.front();
    EXPECT_EQ(partition.GetDataHandle(),
              "PartitionsRepositoryTest-partitionId");
    EXPECT_EQ(partition.GetVersion().value_or(0), 42);
    EXPECT_EQ(partition.GetPartition(), "1111");

    Mock::VerifyAndClearExpectations(cache.get());
  }

  {
    SCOPED_TRACE("Network error at lookup state propagated to the user");
    setup_online_only_mocks();

    EXPECT_CALL(*network,
                Send(IsGetRequest(OLP_SDK_URL_LOOKUP_METADATA), _, _, _, _))
        .WillOnce(NetworkMock::ReturnHttpResponse(
            olp::http::NetworkResponse().WithStatus(
                olp::http::HttpStatusCode::UNAUTHORIZED),
            "Inappropriate"));

    client::CancellationContext context;
    auto response = repository::PartitionsRepository::GetPartitionById(
        catalog_hrn, layer_id, context,
        DataRequest(request).WithFetchOption(OnlineOnly), settings);

    EXPECT_FALSE(response.IsSuccessful());
    EXPECT_EQ(response.GetError().GetErrorCode(),
              olp::client::ErrorCode::AccessDenied);
    Mock::VerifyAndClearExpectations(network.get());
  }
  {
    SCOPED_TRACE("Network error at partition state propagated to the user");
    setup_online_only_mocks();
    setup_positive_metadata_mocks();

    EXPECT_CALL(*network,
                Send(IsGetRequest(OLP_SDK_URL_PARTITION_BY_ID), _, _, _, _))
        .WillOnce(NetworkMock::ReturnHttpResponse(
            olp::http::NetworkResponse().WithStatus(
                olp::http::HttpStatusCode::UNAUTHORIZED),
            "{Inappropriate}"));

    client::CancellationContext context;
    auto response = repository::PartitionsRepository::GetPartitionById(
        catalog_hrn, layer_id, context,
        DataRequest(request).WithFetchOption(OnlineOnly), settings);

    EXPECT_FALSE(response.IsSuccessful());
    EXPECT_EQ(response.GetError().GetErrorCode(),
              olp::client::ErrorCode::AccessDenied);
    Mock::VerifyAndClearExpectations(network.get());
  }
  {
    SCOPED_TRACE(
        "Network error 403 clears cache and is propagated to the user");
    setup_online_only_mocks();
    setup_positive_metadata_mocks();
    EXPECT_CALL(*cache, Get(cache_key, _))
        .Times(1)
        .WillOnce(Return(boost::any()));

    EXPECT_CALL(*network,
                Send(IsGetRequest(OLP_SDK_URL_PARTITION_BY_ID), _, _, _, _))
        .WillOnce(NetworkMock::ReturnHttpResponse(
            olp::http::NetworkResponse().WithStatus(
                olp::http::HttpStatusCode::FORBIDDEN),
            "{Inappropriate}"));

    client::CancellationContext context;
    auto response = repository::PartitionsRepository::GetPartitionById(
        catalog_hrn, layer_id, context,
        DataRequest(request).WithFetchOption(OnlineOnly), settings);

    EXPECT_FALSE(response.IsSuccessful());
    EXPECT_EQ(response.GetError().GetErrorCode(),
              olp::client::ErrorCode::AccessDenied);
    Mock::VerifyAndClearExpectations(network.get());
  }

  {
    SCOPED_TRACE(
        "Network request cancelled by network internally at lookup state");
    setup_online_only_mocks();

    client::CancellationContext context;
    EXPECT_CALL(*network,
                Send(IsGetRequest(OLP_SDK_URL_LOOKUP_METADATA), _, _, _, _))
        .Times(1)
        .WillOnce([=](olp::http::NetworkRequest request,
                      olp::http::Network::Payload payload,
                      olp::http::Network::Callback callback,
                      olp::http::Network::HeaderCallback header_callback,
                      olp::http::Network::DataCallback data_callback)
                      -> olp::http::SendOutcome {
          return olp::http::SendOutcome(olp::http::ErrorCode::CANCELLED_ERROR);
        });

    auto response = repository::PartitionsRepository::GetPartitionById(
        catalog_hrn, layer_id, context,
        DataRequest(request).WithFetchOption(OnlineOnly), settings);

    EXPECT_FALSE(response.IsSuccessful());
    EXPECT_EQ(response.GetError().GetErrorCode(),
              olp::client::ErrorCode::Cancelled);
    Mock::VerifyAndClearExpectations(network.get());
  }
  {
    SCOPED_TRACE(
        "Network request cancelled by network internally at partition state");
    setup_online_only_mocks();
    setup_positive_metadata_mocks();

    client::CancellationContext context;
    EXPECT_CALL(*network,
                Send(IsGetRequest(OLP_SDK_URL_PARTITION_BY_ID), _, _, _, _))
        .Times(1)
        .WillOnce([=](olp::http::NetworkRequest request,
                      olp::http::Network::Payload payload,
                      olp::http::Network::Callback callback,
                      olp::http::Network::HeaderCallback header_callback,
                      olp::http::Network::DataCallback data_callback)
                      -> olp::http::SendOutcome {
          return olp::http::SendOutcome(olp::http::ErrorCode::CANCELLED_ERROR);
        });

    auto response = repository::PartitionsRepository::GetPartitionById(
        catalog_hrn, layer_id, context,
        DataRequest(request).WithFetchOption(OnlineOnly), settings);

    EXPECT_FALSE(response.IsSuccessful());
    EXPECT_EQ(response.GetError().GetErrorCode(),
              olp::client::ErrorCode::Cancelled);
    Mock::VerifyAndClearExpectations(network.get());
  }

  {
    SCOPED_TRACE("Network request timed out at lookup state");
    setup_online_only_mocks();

    client::CancellationContext context;
    EXPECT_CALL(*network,
                Send(IsGetRequest(OLP_SDK_URL_LOOKUP_METADATA), _, _, _, _))
        .Times(1)
        .WillOnce([=](olp::http::NetworkRequest request,
                      olp::http::Network::Payload payload,
                      olp::http::Network::Callback callback,
                      olp::http::Network::HeaderCallback header_callback,
                      olp::http::Network::DataCallback data_callback)
                      -> olp::http::SendOutcome {
          // note no network response thread spawns
          constexpr auto unused_request_id = 12;
          return olp::http::SendOutcome(unused_request_id);
        });
    EXPECT_CALL(*network, Cancel(_)).Times(1).WillOnce(Return());

    auto response = repository::PartitionsRepository::GetPartitionById(
        catalog_hrn, layer_id, context,
        DataRequest(request).WithFetchOption(OnlineOnly), settings);

    EXPECT_FALSE(response.IsSuccessful());
    EXPECT_EQ(response.GetError().GetErrorCode(),
              olp::client::ErrorCode::RequestTimeout);
    Mock::VerifyAndClearExpectations(network.get());
  }
  {
    SCOPED_TRACE("Network request timed out at partition state");
    setup_online_only_mocks();
    setup_positive_metadata_mocks();

    client::CancellationContext context;
    EXPECT_CALL(*network,
                Send(IsGetRequest(OLP_SDK_URL_PARTITION_BY_ID), _, _, _, _))
        .Times(1)
        .WillOnce([=](olp::http::NetworkRequest request,
                      olp::http::Network::Payload payload,
                      olp::http::Network::Callback callback,
                      olp::http::Network::HeaderCallback header_callback,
                      olp::http::Network::DataCallback data_callback)
                      -> olp::http::SendOutcome {
          // note no network response thread spawns
          constexpr auto unused_request_id = 12;
          return olp::http::SendOutcome(unused_request_id);
        });
    EXPECT_CALL(*network, Cancel(_)).Times(1).WillOnce(Return());

    auto response = repository::PartitionsRepository::GetPartitionById(
        catalog_hrn, layer_id, context,
        DataRequest(request).WithFetchOption(OnlineOnly), settings);

    EXPECT_FALSE(response.IsSuccessful());
    EXPECT_EQ(response.GetError().GetErrorCode(),
              olp::client::ErrorCode::RequestTimeout);
    Mock::VerifyAndClearExpectations(network.get());
  }

  {
    SCOPED_TRACE("Network request cancelled by user at lookup state");
    setup_online_only_mocks();

    client::CancellationContext context;
    EXPECT_CALL(*network,
                Send(IsGetRequest(OLP_SDK_URL_LOOKUP_METADATA), _, _, _, _))
        .Times(1)
        .WillOnce(
            [=, &context](olp::http::NetworkRequest request,
                          olp::http::Network::Payload payload,
                          olp::http::Network::Callback callback,
                          olp::http::Network::HeaderCallback header_callback,
                          olp::http::Network::DataCallback data_callback)
                -> olp::http::SendOutcome {
              // spawn a 'user' response of cancelling
              std::thread([&context]() { context.CancelOperation(); }).detach();

              // note no network response thread spawns

              constexpr auto unused_request_id = 12;
              return olp::http::SendOutcome(unused_request_id);
            });
    EXPECT_CALL(*network, Cancel(_)).Times(1).WillOnce(Return());

    auto response = repository::PartitionsRepository::GetPartitionById(
        catalog_hrn, layer_id, context,
        DataRequest(request).WithFetchOption(OnlineOnly), settings);

    EXPECT_FALSE(response.IsSuccessful());
    EXPECT_EQ(response.GetError().GetErrorCode(),
              olp::client::ErrorCode::Cancelled);
    Mock::VerifyAndClearExpectations(network.get());
  }
  {
    SCOPED_TRACE("Network request cancelled by user at patition state");
    setup_online_only_mocks();
    setup_positive_metadata_mocks();

    client::CancellationContext context;
    EXPECT_CALL(*network,
                Send(IsGetRequest(OLP_SDK_URL_PARTITION_BY_ID), _, _, _, _))
        .Times(1)
        .WillOnce(
            [=, &context](olp::http::NetworkRequest request,
                          olp::http::Network::Payload payload,
                          olp::http::Network::Callback callback,
                          olp::http::Network::HeaderCallback header_callback,
                          olp::http::Network::DataCallback data_callback)
                -> olp::http::SendOutcome {
              // spawn a 'user' response of cancelling
              std::thread([&context]() { context.CancelOperation(); }).detach();

              // note no network response thread spawns

              constexpr auto unused_request_id = 12;
              return olp::http::SendOutcome(unused_request_id);
            });
    EXPECT_CALL(*network, Cancel(_)).Times(1).WillOnce(Return());

    auto response = repository::PartitionsRepository::GetPartitionById(
        catalog_hrn, layer_id, context,
        DataRequest(request).WithFetchOption(OnlineOnly), settings);

    EXPECT_FALSE(response.IsSuccessful());
    EXPECT_EQ(response.GetError().GetErrorCode(),
              olp::client::ErrorCode::Cancelled);
    Mock::VerifyAndClearExpectations(network.get());
  }

  {
    SCOPED_TRACE("Network request cancelled before execution setup");
    setup_online_only_mocks();

    client::CancellationContext context;
    context.CancelOperation();
    auto response = repository::PartitionsRepository::GetPartitionById(
        catalog_hrn, layer_id, context,
        DataRequest(request).WithFetchOption(OnlineOnly), settings);

    EXPECT_FALSE(response.IsSuccessful());
    EXPECT_EQ(response.GetError().GetErrorCode(),
              olp::client::ErrorCode::Cancelled);
    Mock::VerifyAndClearExpectations(network.get());
  }
}
}  // namespace