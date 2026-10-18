#ifndef MLVC_APPLICATION_CLI_CONFIG_TABLE_H_
#define MLVC_APPLICATION_CLI_CONFIG_TABLE_H_

#include "mlvc/application/cli/decoder_app.h"
#include "mlvc/application/cli/encoder_app.h"
#include "toml++/toml.h"

namespace mlvc {

// Apply the same schema and value checks as the single-stream file loaders.
EncoderApplicationConfig ParseEncoderConfig(const toml::table& config);
DecoderApplicationConfig ParseDecoderConfig(const toml::table& config);

}  // namespace mlvc

#endif  // MLVC_APPLICATION_CLI_CONFIG_TABLE_H_
