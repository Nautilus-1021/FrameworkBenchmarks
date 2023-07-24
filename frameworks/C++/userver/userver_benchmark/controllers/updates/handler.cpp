#include "handler.hpp"

#include <userver/components/component_context.hpp>
#include <userver/formats/serialize/common_containers.hpp>
#include <userver/storages/postgres/postgres.hpp>

#include <boost/container/small_vector.hpp>

namespace userver_techempower::updates {

namespace {

constexpr const char* kUpdateQueryStr{R"(
UPDATE World w SET
  randomNumber = new_numbers.randomNumber
FROM ( SELECT
  UNNEST($1) as id,
  UNNEST($2) as randomNumber
) new_numbers
WHERE w.id = new_numbers.id
)"};

constexpr std::size_t kBestConcurrencyWildGuess = 128;

}  // namespace

Handler::Handler(const userver::components::ComponentConfig& config,
                 const userver::components::ComponentContext& context)
    : userver::server::handlers::HttpHandlerJsonBase{config, context},
      pg_{context.FindComponent<userver::components::Postgres>("hello-world-db")
              .GetCluster()},
      query_arg_name_{"queries"},
      update_query_{kUpdateQueryStr},
      semaphore_{kBestConcurrencyWildGuess} {}

userver::formats::json::Value Handler::HandleRequestJsonThrow(
    const userver::server::http::HttpRequest& request,
    const userver::formats::json::Value&,
    userver::server::request::RequestContext&) const {
  const auto queries =
      db_helpers::ParseParamFromQuery(request, query_arg_name_);

  return GetResponse(queries);
}

userver::formats::json::Value Handler::GetResponse(int queries) const {
  // userver's PG doesn't accept boost::small_vector as an input, sadly
  std::vector<db_helpers::WorldTableRow> values(queries);
  for (auto& value : values) {
    value.id = db_helpers::GenerateRandomId();
  }
  // we have to sort ids to not deadlock in update
  std::sort(values.begin(), values.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.id < rhs.id; });

  const auto lock = semaphore_.Acquire();
  auto transaction = pg_->Begin(db_helpers::kClusterHostType, {});
  for (auto& value : values) {
    value.random_number = pg_->Execute(db_helpers::kClusterHostType,
                                       db_helpers::kSelectRowQuery, value.id)
                              .AsSingleRow<db_helpers::WorldTableRow>(
                                  userver::storages::postgres::kRowTag)
                              .random_number;
  }

  auto json_result =
      userver::formats::json::ValueBuilder{values}.ExtractValue();

  for (auto& value : values) {
    value.random_number = db_helpers::GenerateRandomValue();
  }

  userver::storages::postgres::io::SplitContainerByColumns(values,
                                                           values.size())
      .Perform([this](const auto&... args) {
        pg_->Execute(db_helpers::kClusterHostType, update_query_, args...);
      });

  return json_result;
}

}  // namespace userver_techempower::updates
