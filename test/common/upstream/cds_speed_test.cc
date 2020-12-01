// Note: this should be run with --compilation_mode=opt, and would benefit from a
// quiescent system with disabled cstate power management.

#include "envoy/config/cluster/v3/cluster.pb.h"
#include "envoy/config/core/v3/health_check.pb.h"
#include "envoy/config/endpoint/v3/endpoint.pb.h"
#include "envoy/config/endpoint/v3/endpoint_components.pb.h"
#include "envoy/service/discovery/v3/discovery.pb.h"
#include "envoy/stats/scope.h"

#include "common/config/grpc_mux_impl.h"
#include "common/config/grpc_subscription_impl.h"
#include "common/config/utility.h"
#include "common/singleton/manager_impl.h"
#include "common/upstream/eds.h"

#include "server/transport_socket_config_impl.h"

#include "test/benchmark/main.h"
#include "test/common/upstream/utility.h"
#include "test/mocks/local_info/mocks.h"
#include "test/mocks/protobuf/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/server/admin.h"
#include "test/mocks/server/instance.h"
#include "test/mocks/ssl/mocks.h"
#include "test/mocks/upstream/cluster_manager.h"
#include "test/test_common/test_runtime.h"
#include "test/test_common/utility.h"

#include "benchmark/benchmark.h"

using ::benchmark::State;
using Envoy::benchmark::skipExpensiveBenchmarks;

namespace Envoy {
namespace Upstream {

class CdsSpeedTest {
public:
  CdsSpeedTest(State& state, bool v2_config)
      : state_(state), v2_config_(v2_config),
        type_url_(v2_config_
                      ? "type.googleapis.com/envoy.api.v2.ClusterLoadAssignment"
                      : "type.googleapis.com/envoy.config.endpoint.v3.ClusterLoadAssignment"),
        subscription_stats_(Config::Utility::generateStats(stats_)),
        api_(Api::createApiForTest(stats_)), async_client_(new Grpc::MockAsyncClient()),
        grpc_mux_(new Config::GrpcMuxImpl(
            local_info_, std::unique_ptr<Grpc::MockAsyncClient>(async_client_), dispatcher_,
            *Protobuf::DescriptorPool::generated_pool()->FindMethodByName(
                "envoy.service.endpoint.v3.EndpointDiscoveryService.StreamEndpoints"),
            envoy::config::core::v3::ApiVersion::AUTO, random_, stats_, {}, true)) {

    resetCluster(R"EOF(
      name: name
      connect_timeout: 0.25s
      type: EDS
      eds_cluster_config:
        service_name: fare
        eds_config:
          api_config_source:
            cluster_names:
            - eds
            refresh_delay: 1s
    )EOF",
                 Envoy::Upstream::Cluster::InitializePhase::Secondary);

    EXPECT_CALL(*cm_.subscription_factory_.subscription_, start(_, _));
    cluster_->initialize([this] { initialized_ = true; });
    EXPECT_CALL(*async_client_, startRaw(_, _, _, _)).WillOnce(testing::Return(&async_stream_));
    subscription_->start({"fare"});
  }

  void resetCluster(const std::string& yaml_config, Cluster::InitializePhase initialize_phase) {
    local_info_.node_.mutable_locality()->set_zone("us-east-1a");
    eds_cluster_ = parseClusterFromV3Yaml(yaml_config);
    Envoy::Stats::ScopePtr scope = stats_.createScope(fmt::format(
        "cluster.{}.",
        eds_cluster_.alt_stat_name().empty() ? eds_cluster_.name() : eds_cluster_.alt_stat_name()));
    Envoy::Server::Configuration::TransportSocketFactoryContextImpl factory_context(
        admin_, ssl_context_manager_, *scope, cm_, local_info_, dispatcher_, stats_,
        singleton_manager_, tls_, validation_visitor_, *api_);
    cluster_ = std::make_shared<EdsClusterImpl>(eds_cluster_, runtime_, factory_context,
                                                std::move(scope), false);
    EXPECT_EQ(initialize_phase, cluster_->initializePhase());
    eds_callbacks_ = cm_.subscription_factory_.callbacks_;
    subscription_ = std::make_unique<Config::GrpcSubscriptionImpl>(
        grpc_mux_, *eds_callbacks_, resource_decoder_, subscription_stats_, type_url_, dispatcher_,
        std::chrono::milliseconds(), false);
  }

  void clusterHelper(bool ignore_unknown_dynamic_fields, size_t num_clusters) {
    state_.PauseTiming();

    auto response = std::make_unique<envoy::service::discovery::v3::DiscoveryResponse>();
    response->set_type_url(type_url_);
    response->set_version_info(fmt::format("version-{}", version_++));

    // make a pile of dynamic clusters and add them to the response
    for (size_t i = 0; i < num_clusters; ++i) {
      envoy::config::endpoint::v3::ClusterLoadAssignment cluster_load_assignment;
      cluster_load_assignment.set_cluster_name("fare_" + std::to_string(i));

      auto* endpoints = cluster_load_assignment.add_endpoints();
      endpoints->set_priority(1);
      auto* locality = endpoints->mutable_locality();
      locality->set_region("region");
      locality->set_zone("zone");
      locality->set_sub_zone("sub_zone");

      auto* resource = response->mutable_resources()->Add();
      resource->PackFrom(cluster_load_assignment);
      if (v2_config_) {
        RELEASE_ASSERT(resource->type_url() ==
                           "type.googleapis.com/envoy.config.endpoint.v3.ClusterLoadAssignment",
                       "");
        resource->set_type_url("type.googleapis.com/envoy.api.v2.ClusterLoadAssignment");
      }
    }
    validation_visitor_.setSkipValidation(ignore_unknown_dynamic_fields);

    state_.SetComplexityN(num_clusters);
    state_.ResumeTiming();
    grpc_mux_->grpcStreamForTest().onReceiveMessage(std::move(response));
  }

  TestDeprecatedV2Api _deprecated_v2_api_;
  State& state_;
  const bool v2_config_;
  const std::string type_url_;
  uint64_t version_{};
  bool initialized_{};
  Stats::IsolatedStoreImpl stats_;
  Config::SubscriptionStats subscription_stats_;
  Ssl::MockContextManager ssl_context_manager_;
  envoy::config::cluster::v3::Cluster eds_cluster_;
  NiceMock<MockClusterManager> cm_;
  NiceMock<Event::MockDispatcher> dispatcher_;
  EdsClusterImplSharedPtr cluster_;
  Config::SubscriptionCallbacks* eds_callbacks_{};
  Config::OpaqueResourceDecoderImpl<envoy::config::endpoint::v3::ClusterLoadAssignment>
      resource_decoder_{validation_visitor_, "cluster_name"};
  NiceMock<Random::MockRandomGenerator> random_;
  NiceMock<Runtime::MockLoader> runtime_;
  NiceMock<LocalInfo::MockLocalInfo> local_info_;
  NiceMock<Server::MockAdmin> admin_;
  Singleton::ManagerImpl singleton_manager_{Thread::threadFactoryForTest()};
  NiceMock<ThreadLocal::MockInstance> tls_;
  ProtobufMessage::MockValidationVisitor validation_visitor_;
  Api::ApiPtr api_;
  Grpc::MockAsyncClient* async_client_;
  NiceMock<Grpc::MockAsyncStream> async_stream_;
  Config::GrpcMuxImplSharedPtr grpc_mux_;
  Config::GrpcSubscriptionImplPtr subscription_;
};

} // namespace Upstream
} // namespace Envoy

static void addClusters(State& state) {
  for (auto _ : state) { // NOLINT(clang-analyzer-deadcode.DeadStores)
    Envoy::Upstream::CdsSpeedTest speed_test(state, state.range(0));
    // if we've been instructed to skip tests, only run once no matter the argument:
    uint32_t clusters = skipExpensiveBenchmarks() ? 1 : state.range(2);
    speed_test.clusterHelper(state.range(1), clusters);
  }
}

BENCHMARK(addClusters)
    ->Ranges({{false, true}, {false, true}, {64, 100000}})
    ->Unit(benchmark::kMillisecond)
    ->Complexity();

// Look for suboptimal behavior when receiving two identical updates
static void duplicateUpdate(State& state) {
  for (auto _ : state) { // NOLINT(clang-analyzer-deadcode.DeadStores)
    Envoy::Upstream::CdsSpeedTest speed_test(state, false);
    uint32_t clusters = skipExpensiveBenchmarks() ? 1 : state.range(0);

    speed_test.clusterHelper(true, clusters);
    speed_test.clusterHelper(true, clusters);
  }
}

BENCHMARK(duplicateUpdate)->Range(64, 100000)->Unit(benchmark::kMillisecond)->Complexity();
