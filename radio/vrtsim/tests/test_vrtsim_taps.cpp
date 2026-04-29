/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "common_lib.h"
#include <gtest/gtest.h>
#include "common/config/config_userapi.h"
#include <thread>
#include <vector>
#include <complex>
#include <nanomsg/nn.h>
#include <nanomsg/pubsub.h>
#include "taps_generated.h"

configmodule_interface_t *uniqCfg = NULL;

extern "C" {
#include "common/config/config_userapi.h"
#include "openair1/SIMULATION/TOOLS/sim.h"
extern int device_init(openair0_device_t *device, openair0_config_t *openair0_cfg);
extern void vrtsim_produce_samples(openair0_device_t *device, size_t num_samples);
static softmodem_params_t softmodem_params;
softmodem_params_t *get_softmodem_params(void)
{
  return &softmodem_params;
}
void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  fprintf(stderr, "FATAL: %s at %s:%s:%d\n", s, file, function, line);
  exit(EXIT_FAILURE);
}
}

class TapsProducer {
  int sock;

 public:
  TapsProducer(const char *url)
  {
    sock = nn_socket(AF_SP, NN_PUB);
    int ret = nn_bind(sock, url);
    if (ret < 0) {
      fprintf(stderr, "nn_bind failed: %s\n", nn_strerror(nn_errno()));
      exit(1);
    }
  }
  ~TapsProducer()
  {
    nn_close(sock);
  }
  void send_taps(uint32_t id, uint32_t tx, uint32_t rx, uint32_t len, const std::vector<std::complex<float>> &taps)
  {
    flatbuffers::FlatBufferBuilder builder;
    auto taps_vec = builder.CreateVector(reinterpret_cast<const float *>(taps.data()), taps.size() * 2);
    auto taps_obj = Phy::CreateTaps(builder, id, tx, rx, len, taps_vec);
    builder.Finish(taps_obj);
    nn_send(sock, builder.GetBufferPointer(), builder.GetSize(), 0);
  }
};

struct TapsAntParams {
  int gnb_tx;
  int gnb_rx;
  int ue_tx;
  int ue_rx;
  std::string ue_ant_str;
  TapsAntParams(int gt, int gr, int ut, int ur, std::string s) : gnb_tx(gt), gnb_rx(gr), ue_tx(ut), ue_rx(ur), ue_ant_str(s)
  {
  }
};

class VRTSTapsTest : public ::testing::TestWithParam<TapsAntParams> {
 protected:
  configmodule_interface_t *cfg1 = nullptr;
  configmodule_interface_t *cfg2 = nullptr;
  openair0_device_t server_device = {0};
  openair0_device_t client_device = {0};
  openair0_config_t server_config = {0};
  openair0_config_t client_config = {0};
  std::string server_taps_url;
  std::string client_taps_url;

  void SetUp() override
  {
    auto p = GetParam();
    std::string tag = std::to_string(p.gnb_tx) + "_" + p.ue_ant_str;
    server_taps_url = "ipc:///tmp/server_taps_" + tag + ".ipc";
    client_taps_url = "ipc:///tmp/client_taps_" + tag + ".ipc";
    std::string shm_name = "shm_taps_" + tag;

    // Setup server
    const char *server_argv[] = {"--vrtsim.role",
                                 "server",
                                 "--vrtsim.shm_channel_name",
                                 shm_name.c_str(),
                                 "--vrtsim.disable-timing-thread",
                                 "1",
                                 "--vrtsim.taps-socket",
                                 server_taps_url.c_str(),
                                 "--vrtsim.ue_config.[0].antennas",
                                 p.ue_ant_str.c_str()};
    cfg1 = load_configmodule(sizeof(server_argv) / sizeof(char *), (char **)server_argv, CONFIG_ENABLECMDLINEONLY);
    uniqCfg = cfg1;
    server_config.tx_num_channels = p.gnb_tx;
    server_config.rx_num_channels = p.gnb_rx;
    server_config.sample_rate = 30.72e6;
    ASSERT_EQ(device_init(&server_device, &server_config), 0);
    ASSERT_EQ(server_device.trx_start_func(&server_device), 0);

    // Setup client
    const char *client_argv[] =
        {"--vrtsim.role", "client", "--vrtsim.shm_channel_name", shm_name.c_str(), "--vrtsim.taps-socket", client_taps_url.c_str()};
    cfg2 = load_configmodule(sizeof(client_argv) / sizeof(char *), (char **)client_argv, CONFIG_ENABLECMDLINEONLY);
    uniqCfg = cfg2;
    client_config.tx_num_channels = p.ue_tx;
    client_config.rx_num_channels = p.ue_rx;
    client_config.sample_rate = 30.72e6;
    ASSERT_EQ(device_init(&client_device, &client_config), 0);
    ASSERT_EQ(client_device.trx_start_func(&client_device), 0);
  }

  void TearDown() override
  {
    if (server_device.trx_end_func)
      server_device.trx_end_func(&server_device);
    if (client_device.trx_end_func)
      client_device.trx_end_func(&client_device);
    if (cfg1)
      end_configmodule(cfg1);
    if (cfg2)
      end_configmodule(cfg2);
  }
};

TEST_P(VRTSTapsTest, TapsDLSum)
{
  auto p = GetParam();
  TapsProducer server_producer(server_taps_url.c_str());
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  const uint32_t L = 16;
  // DL (gNB -> UE): nb_tx = gnb_tx, nb_rx = ue_rx.
  std::vector<std::complex<float>> taps(p.gnb_tx * p.ue_rx * L, {0.0f, 0.0f});

  // All TX channels contribute to RX channel 0 with no delay (tap 0 = 1.0)
  for (int tx = 0; tx < p.gnb_tx; tx++) {
    int link = (0 + p.ue_rx * tx); // rx=0, num_rx=ue_rx
    taps[link * L + 0] = {1.0f, 0.0f};
  }

  server_producer.send_taps(0, p.gnb_tx, p.ue_rx, L, taps);
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  const int nsamps = 1024;
  std::vector<std::vector<c16_t>> tx_chan_samples(p.gnb_tx, std::vector<c16_t>(nsamps));
  std::vector<void *> tx_ptrs(p.gnb_tx);

  for (int ch = 0; ch < p.gnb_tx; ch++) {
    for (int i = 0; i < nsamps; i++) {
      tx_chan_samples[ch][i] = {(int16_t)(i + 1 + ch * 100), (int16_t) - (i + 1 + ch * 100)};
    }
    tx_ptrs[ch] = tx_chan_samples[ch].data();
  }

  // Write DL samples
  ASSERT_EQ(server_device.trx_write_func(&server_device, 0, tx_ptrs.data(), nsamps, p.gnb_tx, 0), nsamps);

  vrtsim_produce_samples(&server_device, nsamps);

  // Read on client (DL)
  std::vector<std::vector<c16_t>> rx_chan_samples(p.ue_rx, std::vector<c16_t>(nsamps));
  std::vector<void *> rx_ptrs(p.ue_rx);
  for (int ch = 0; ch < p.ue_rx; ch++)
    rx_ptrs[ch] = rx_chan_samples[ch].data();

  openair0_timestamp_t rx_ts;
  ASSERT_EQ(client_device.trx_read_func(&client_device, &rx_ts, rx_ptrs.data(), nsamps, p.ue_rx), nsamps);

  // Verify DL Sum for rx_ch 0
  for (int i = 0; i < nsamps; i++) {
    int32_t expected_r = 0;
    int32_t expected_i = 0;
    for (int tx_ch = 0; tx_ch < p.gnb_tx; tx_ch++) {
      expected_r += tx_chan_samples[tx_ch][i].r;
      expected_i += tx_chan_samples[tx_ch][i].i;
    }
    EXPECT_EQ(rx_chan_samples[0][i].r, (int16_t)expected_r) << "DL Sum Mismatch at rx_ch 0 index " << i;
    EXPECT_EQ(rx_chan_samples[0][i].i, (int16_t)expected_i) << "DL Sum Mismatch at rx_ch 0 index " << i;
  }
}

TEST_P(VRTSTapsTest, TapsULDelay)
{
  auto p = GetParam();
  TapsProducer client_producer(client_taps_url.c_str());
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  const uint32_t L = 16;
  // UL (UE -> gNB): nb_tx = ue_tx, nb_rx = gnb_rx.
  std::vector<std::complex<float>> taps(p.ue_tx * p.gnb_rx * L, {0.0f, 0.0f});

  // UE TX 0 -> gNB RX 0 (no delay)
  // UE TX 0 -> gNB RX 1 (delayed L-1) if gnb_rx > 1
  taps[(0 + p.gnb_rx * 0) * L + 0] = {1.0f, 0.0f};
  if (p.gnb_rx > 1) {
    taps[(1 + p.gnb_rx * 0) * L + (L - 1)] = {1.0f, 0.0f};
  }

  client_producer.send_taps(0, p.ue_tx, p.gnb_rx, L, taps);
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  const int nsamps = 1024;
  std::vector<std::vector<c16_t>> tx_chan_samples(p.ue_tx, std::vector<c16_t>(nsamps));
  std::vector<void *> tx_ptrs(p.ue_tx);
  for (int ch = 0; ch < p.ue_tx; ch++) {
    for (int i = 0; i < nsamps; i++) {
      tx_chan_samples[ch][i] = {(int16_t)(i + 500), (int16_t) - (i + 500)};
    }
    tx_ptrs[ch] = tx_chan_samples[ch].data();
  }

  // Write UL samples from client
  ASSERT_EQ(client_device.trx_write_func(&client_device, 0, tx_ptrs.data(), nsamps, p.ue_tx, 0), nsamps);

  vrtsim_produce_samples(&server_device, nsamps);

  // Read on server (UL)
  std::vector<std::vector<c16_t>> rx_chan_samples(p.gnb_rx, std::vector<c16_t>(nsamps));
  std::vector<void *> rx_ptrs(p.gnb_rx);
  for (int ch = 0; ch < p.gnb_rx; ch++)
    rx_ptrs[ch] = rx_chan_samples[ch].data();

  openair0_timestamp_t rx_ts;
  ASSERT_EQ(server_device.trx_read_func(&server_device, &rx_ts, rx_ptrs.data(), nsamps, p.gnb_rx), nsamps);

  // Verify UL RX 0 (no delay)
  for (int i = 0; i < nsamps; i++) {
    EXPECT_EQ(rx_chan_samples[0][i].r, tx_chan_samples[0][i].r) << "UL RX 0 Mismatch at " << i;
  }
  // Verify UL RX 1 (delayed) if present
  if (p.gnb_rx > 1) {
    for (int i = L - 1; i < nsamps; i++) {
      EXPECT_EQ(rx_chan_samples[1][i].r, tx_chan_samples[0][i - (L - 1)].r) << "UL RX 1 Delay Mismatch at " << i;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(AntennaVariations,
                         VRTSTapsTest,
                         ::testing::Values(TapsAntParams(1, 1, 1, 1, "1x1"),
                                           TapsAntParams(2, 2, 2, 2, "2x2"),
                                           TapsAntParams(8, 8, 1, 1, "1x1"),
                                           TapsAntParams(8, 8, 2, 2, "2x2")));

int main(int argc, char **argv)
{
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
