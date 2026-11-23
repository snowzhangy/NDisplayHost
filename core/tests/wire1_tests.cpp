// WIRE.md v1 codec + FEC unit tests (see wire1.h, wire1_msg.h, fec.h).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "wvd/fec.h"
#include "wvd/wire1.h"
#include "wvd/wire1_msg.h"

using namespace wvd;

static int failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);            \
      failures++;                                                            \
    }                                                                        \
  } while (0)

static void TestPbBasics() {
  // Writer: proto3 omits defaults.
  pb::Writer w;
  w.u32(1, 0);
  CHECK(w.bytes.empty());
  w.boolean(2, false);
  CHECK(w.bytes.empty());

  pb::Writer w2;
  w2.u32(1, 1);
  CHECK(w2.bytes.size() == 2 && w2.bytes[0] == (1 << 3 | 0) && w2.bytes[1] == 1);

  // sint32 zigzag: -1 -> 1, 1 -> 2, -2 -> 3.
  pb::Writer w3;
  w3.s32(1, -1);
  CHECK(w3.bytes.size() == 2 && w3.bytes[1] == 1);
  pb::Reader r3(w3.bytes.data(), w3.bytes.size());
  int f;
  uint8_t wt;
  CHECK(r3.nextField(f, wt) && f == 1 && wt == pb::kVarint);
  int32_t v = 0;
  CHECK(r3.s32(v) && v == -1);

  // float round trip (including negative zero, which is written).
  pb::Writer w4;
  w4.flt(5, -0.0f);
  CHECK(!w4.bytes.empty());
  pb::Reader r4(w4.bytes.data(), w4.bytes.size());
  CHECK(r4.nextField(f, wt) && f == 5 && wt == pb::kFixed32);
  float fv = 1.f;
  CHECK(r4.flt(fv));
  uint32_t fbits = 0;
  std::memcpy(&fbits, &fv, 4);
  CHECK(fbits == 0x80000000u);  // negative zero survives

  // Packed repeated uint32 decodes both packed and unpacked.
  pb::Writer w5;
  w5.packedU32(9, {150, 300, 450});
  pb::Reader r5(w5.bytes.data(), w5.bytes.size());
  std::vector<uint32_t> got;
  CHECK(r5.nextField(f, wt) && f == 9 && wt == pb::kLen);
  CHECK(r5.repeatedU32(wt, got) && got == std::vector<uint32_t>({150, 300, 450}));

  // Unknown fields are skipped.
  pb::Writer w6;
  w6.u32(1, 7);
  w6.str(99, "future");
  pb::Reader r6(w6.bytes.data(), w6.bytes.size());
  uint32_t a = 0;
  CHECK(r6.nextField(f, wt) && f == 1);
  CHECK(r6.u32(a) && a == 7);
  CHECK(r6.nextField(f, wt) && f == 99);
  CHECK(r6.skip(wt));
  CHECK(r6.atEnd());
}

static void TestFraming() {
  msg::Envelope in;
  in.kind = msg::Envelope::Kind::Ping;
  in.ping.clientSendUs = 0x0102030405060708ull;
  in.ping.seq = 42;
  auto body = in.encode();
  auto frame = framing::frame(body);

  framing::Deframer d(true);
  // Preamble then frame, fed one byte at a time; one envelope at the end.
  std::vector<uint8_t> stream(framing::kPreamble, framing::kPreamble + 4);
  stream.insert(stream.end(), frame.begin(), frame.end());
  int got = 0;
  for (size_t i = 0; i < stream.size(); i++) {
    CHECK(d.append(&stream[i], 1));
    std::vector<uint8_t> out;
    while (d.next(out)) {
      got++;
      msg::Envelope env;
      CHECK(env.decode(out.data(), out.size()));
      CHECK(env.kind == msg::Envelope::Kind::Ping);
      CHECK(env.ping.clientSendUs == in.ping.clientSendUs);
      CHECK(env.ping.seq == 42);
    }
  }
  CHECK(got == 1);

  // Bad preamble is a framing violation.
  framing::Deframer bad(true);
  const uint8_t junk[4] = {'X', 'X', 'X', 'X'};
  bad.append(junk, 4);
  std::vector<uint8_t> out;
  CHECK(!bad.next(out));
}

static void TestMessages() {
  // Hello with everything a paired client sets.
  msg::Hello h;
  h.protocolVersion = 1;
  h.deviceId = "uuid-1";
  h.deviceName = "Z\xc3\xa4hne";  // utf8
  h.deviceModel = "Apple Vision Pro";
  h.devicePublicKey.assign(65, 0xAB);
  h.authSignature.assign(70, 0x3D);
  h.decoder.hevc = true;
  h.decoder.maxWidth = 8192;
  h.decoder.maxPixelRate = 1000000000ull;
  h.displayRefreshMhz = {90000, 120000};
  h.thermal = msg::ThermalState::Serious;
  h.maxDatagramSize = 1180;

  msg::Envelope in;
  in.kind = msg::Envelope::Kind::Hello;
  in.hello = h;
  auto bytes = in.encode();
  msg::Envelope outEnv;
  CHECK(outEnv.decode(bytes.data(), bytes.size()));
  CHECK(outEnv.kind == msg::Envelope::Kind::Hello);
  CHECK(outEnv.hello.protocolVersion == 1);
  CHECK(outEnv.hello.deviceId == "uuid-1");
  CHECK(outEnv.hello.deviceName == h.deviceName);
  CHECK(outEnv.hello.devicePublicKey.size() == 65);
  CHECK(outEnv.hello.authSignature.size() == 70);
  CHECK(outEnv.hello.decoder.hevc && outEnv.hello.decoder.maxWidth == 8192);
  CHECK(outEnv.hello.decoder.maxPixelRate == 1000000000ull);
  CHECK(outEnv.hello.displayRefreshMhz == std::vector<uint32_t>({90000, 120000}));
  CHECK(outEnv.hello.thermal == msg::ThermalState::Serious);
  CHECK(outEnv.hello.maxDatagramSize == 1180);

  // An empty oneof member still identifies its kind (InputStreamOpen gen 0).
  msg::Envelope openIn;
  openIn.kind = msg::Envelope::Kind::InputStreamOpen;
  openIn.inputStreamOpen.sessionGen = 0;
  auto openBytes = openIn.encode();
  CHECK(openBytes.size() == 3);  // two-byte key (field 21) + zero length
  msg::Envelope openOut;
  CHECK(openOut.decode(openBytes.data(), openBytes.size()));
  CHECK(openOut.kind == msg::Envelope::Kind::InputStreamOpen);

  // Monitor with negative desktop origin (sint32).
  msg::Envelope layIn;
  layIn.kind = msg::Envelope::Kind::MonitorLayout;
  layIn.monitorLayout.layoutGen = 3;
  msg::Monitor m;
  m.monitorId = 1;
  m.name = "DP-1";
  m.width = 2560;
  m.height = 1600;
  m.refreshMhz = 165000;
  m.desktopX = -2560;
  m.desktopY = -100;
  m.primary = true;
  layIn.monitorLayout.monitors.push_back(m);
  auto layBytes = layIn.encode();
  msg::Envelope layOut;
  CHECK(layOut.decode(layBytes.data(), layBytes.size()));
  CHECK(layOut.kind == msg::Envelope::Kind::MonitorLayout);
  CHECK(layOut.monitorLayout.monitors.size() == 1);
  CHECK(layOut.monitorLayout.monitors[0].desktopX == -2560);
  CHECK(layOut.monitorLayout.monitors[0].desktopY == -100);
  CHECK(layOut.monitorLayout.monitors[0].primary);

  // CodecConfig with empty + non-empty parameter sets (positional).
  msg::Envelope ccIn;
  ccIn.kind = msg::Envelope::Kind::CodecConfig;
  ccIn.codecConfig.monitorId = 1;
  ccIn.codecConfig.codecGen = 2;
  ccIn.codecConfig.codec = msg::Codec::Hevc;
  ccIn.codecConfig.parameterSets = {{1, 2, 3}, {}, {9}};
  ccIn.codecConfig.color.primaries = 1;
  ccIn.codecConfig.color.transfer = 13;
  ccIn.codecConfig.color.fullRange = true;
  auto ccBytes = ccIn.encode();
  msg::Envelope ccOut;
  CHECK(ccOut.decode(ccBytes.data(), ccBytes.size()));
  CHECK(ccOut.codecConfig.parameterSets.size() == 3);
  CHECK(ccOut.codecConfig.parameterSets[1].empty());
  CHECK(ccOut.codecConfig.color.transfer == 13);
  CHECK(ccOut.codecConfig.color.fullRange);

  // InputEvents round trip (oneof inside a repeated message).
  msg::Envelope inIn;
  inIn.kind = msg::Envelope::Kind::InputEvents;
  inIn.input.inputGen = 5;
  msg::InputEvent e1;
  e1.seq = 1;
  e1.kind = msg::InputEvent::Kind::Move;
  e1.move.monitorId = 1;
  e1.move.layoutGen = 2;
  e1.move.x = 100.5f;
  e1.move.y = -0.25f;
  msg::InputEvent e2;
  e2.seq = 2;
  e2.kind = msg::InputEvent::Kind::Key;
  e2.key.hidUsage = 0x04;
  e2.key.down = true;
  msg::InputEvent e3;
  e3.seq = 3;
  e3.kind = msg::InputEvent::Kind::Text;
  e3.text.text = "\xe2\x9c\x93";
  inIn.input.events = {e1, e2, e3};
  auto inBytes = inIn.encode();
  msg::Envelope inOut;
  CHECK(inOut.decode(inBytes.data(), inBytes.size()));
  CHECK(inOut.input.inputGen == 5 && inOut.input.events.size() == 3);
  CHECK(inOut.input.events[0].kind == msg::InputEvent::Kind::Move);
  CHECK(inOut.input.events[0].move.x == 100.5f && inOut.input.events[0].move.y == -0.25f);
  CHECK(inOut.input.events[1].kind == msg::InputEvent::Kind::Key);
  CHECK(inOut.input.events[1].key.hidUsage == 4 && inOut.input.events[1].key.down);
  CHECK(inOut.input.events[2].kind == msg::InputEvent::Kind::Text);
  CHECK(inOut.input.events[2].text.text == "\xe2\x9c\x93");

  // Garbage is rejected, not half-parsed.
  const uint8_t garbage[3] = {0xFF, 0xFF, 0xFF};
  msg::Envelope bad;
  CHECK(!bad.decode(garbage, 3));
}

static void TestDatagrams() {
  dg::MediaShardHeader h;
  h.flags = dg::MediaShardHeader::kRandomAccess | dg::MediaShardHeader::kParity;
  h.monitorId = 2;
  h.sessionGen = 7;
  h.layoutGen = 8;
  h.codecGen = 9;
  h.picSeq = 12345;
  h.contentVer = 54321;
  h.frameBytes = 100000;
  h.blockFirstShard = 0;
  h.shardIndex = 3;
  h.k = 48;
  h.m = 5;
  h.shardBytes = 1152;
  h.totalDataShards = 87;
  h.deadlineBudgetMs = 100;
  h.captureTimeUs = 0xdeadbeefcafeull;
  std::vector<uint8_t> buf(dg::MediaShardHeader::kSize + h.shardBytes, 0);
  h.writeTo(buf.data());
  dg::MediaShardHeader p;
  CHECK(dg::MediaShardHeader::parse(buf.data(), buf.size(), p));
  CHECK(p.picSeq == 12345 && p.k == 48 && p.m == 5 && p.captureTimeUs == h.captureTimeUs);
  CHECK(p.flags == h.flags);

  // Malformed: k+m > 255.
  auto badBuf = buf;
  badBuf[32] = 254;
  badBuf[33] = 2;
  CHECK(!dg::MediaShardHeader::parse(badBuf.data(), badBuf.size(), p));
  // Malformed: frame_bytes outside ((N-1)*shard, N*shard].
  auto badBuf2 = buf;
  badBuf2[24] = 0;
  badBuf2[25] = 0;
  badBuf2[26] = 0x60;  // 0x00600000 = 6291456 > 87*1152
  badBuf2[27] = 0;
  CHECK(!dg::MediaShardHeader::parse(badBuf2.data(), badBuf2.size(), p));
  // Short datagram.
  CHECK(!dg::MediaShardHeader::parse(buf.data(), dg::MediaShardHeader::kSize, p));

  // Nack round trip.
  dg::Nack n;
  n.monitorId = 1;
  n.codecGen = 4;
  n.picSeq = 99;
  n.missing = {0, 17, 48};
  auto nb = dg::Nack::encode(n);
  dg::Nack np;
  CHECK(dg::Nack::parse(nb.data(), nb.size(), np));
  CHECK(np.picSeq == 99 && np.missing == n.missing);

  // FlowHello: magic must match.
  std::vector<uint8_t> fh = {0x10, 0, 0, 0, 0x57, 0x56, 0x44, 0x47, 5, 0, 0, 0};
  dg::FlowHello fhp;
  CHECK(dg::FlowHello::parse(fh.data(), fh.size(), fhp));
  CHECK(fhp.sessionGen == 5);
  fh[4] = 0x00;
  CHECK(!dg::FlowHello::parse(fh.data(), fh.size(), fhp));

  // Cursor datagram length.
  dg::CursorDatagram c;
  c.visible = true;
  c.seq = 12;
  c.x = 1.5f;
  c.y = 2.5f;
  auto cb = dg::CursorDatagram::encode(c);
  CHECK(cb.size() == dg::CursorDatagram::kSize);
  CHECK(cb[0] == kDgCursor && cb[1] == 1);
}

static void TestFec() {
  // Encode/reconstruct: 10 data + 3 parity, lose 3 data shards.
  const int k = 10, m = 3;
  const size_t sb = 37;
  std::vector<std::vector<uint8_t>> data(k, std::vector<uint8_t>(sb));
  for (int i = 0; i < k; i++)
    for (size_t b = 0; b < sb; b++) data[i][b] = static_cast<uint8_t>((i * 31 + b * 7) & 0xFF);
  std::vector<std::vector<uint8_t>> parity(m, std::vector<uint8_t>(sb));
  {
    std::vector<const uint8_t*> dp(k);
    std::vector<uint8_t*> pp(m);
    for (int i = 0; i < k; i++) dp[i] = data[i].data();
    for (int j = 0; j < m; j++) pp[j] = parity[j].data();
    CHECK(fec::Encode(dp.data(), k, pp.data(), m, sb));
  }
  auto original = data;
  // Erase data 2, 5, 9; use parity rows 0 and 2 for two of them, all three here.
  int rows[2] = {0, 2};
  data[2].assign(sb, 0);
  data[5].assign(sb, 0);
  // With parity row 1 also available we could lose 3; test d=2 with rows {0,2}.
  {
    bool present[10] = {true, true, false, true, true, false, true, true, true, true};
    std::vector<const uint8_t*> dp(k);
    for (int i = 0; i < k; i++) dp[i] = data[i].data();
    std::vector<const uint8_t*> pp = {parity[0].data(), parity[2].data()};
    CHECK(fec::Reconstruct(k, dp.data(), present, rows, pp.data(), 2, sb));
  }
  CHECK(data[2] == original[2]);
  CHECK(data[5] == original[5]);

  // Re-encode from recovered data matches parity (matrix identity check).
  {
    std::vector<std::vector<uint8_t>> re(m, std::vector<uint8_t>(sb));
    std::vector<const uint8_t*> dp(k);
    std::vector<uint8_t*> rp(m);
    for (int i = 0; i < k; i++) dp[i] = original[i].data();
    for (int j = 0; j < m; j++) rp[j] = re[j].data();
    CHECK(fec::Encode(dp.data(), k, rp.data(), m, sb));
    CHECK(re == parity);
  }

  // Cauchy property: any k of k+m shards are independent. Reconstruct with
  // every subset of m rows from a single erasure.
  for (int useRow = 0; useRow < m; useRow++) {
    auto d2 = original;
    d2[3].assign(sb, 0);
    bool present[10] = {true, true, true, false, true, true, true, true, true, true};
    std::vector<const uint8_t*> dp(k);
    for (int i = 0; i < k; i++) dp[i] = d2[i].data();
    std::vector<const uint8_t*> pp = {parity[useRow].data()};
    int row = useRow;
    CHECK(fec::Reconstruct(k, dp.data(), present, &row, pp.data(), 1, sb));
    CHECK(d2[3] == original[3]);
  }
}

static void TestPacketizer() {
  fec::PacketizerConfig cfg;
  cfg.shardBytes = 64;
  cfg.maxBlockDataShards = 10;
  cfg.fecRatio = 0.25;

  // 1000 bytes -> N = ceil(1000/64) = 16 shards -> B = 2 blocks of k=8,
  // m = ceil(8*0.25) = 2.
  std::vector<uint8_t> au(1000);
  for (size_t i = 0; i < au.size(); i++) au[i] = static_cast<uint8_t>(i & 0xFF);
  fec::FrameDescriptor desc;
  desc.picSeq = 77;
  desc.randomAccess = true;
  fec::PacketizedFrame pf(desc, au.data(), au.size(), cfg);
  CHECK(pf.totalDataShards() == 16);
  CHECK(pf.blocks().size() == 2);
  CHECK(pf.blocks()[0].k == 8 && pf.blocks()[0].m == 2);
  CHECK(pf.blocks()[1].k == 8 && pf.blocks()[1].m == 2);
  CHECK(pf.dataDatagrams().size() == 16);

  // Every data datagram parses; flags carry RANDOM_ACCESS.
  for (const auto& d : pf.dataDatagrams()) {
    dg::MediaShardHeader h;
    CHECK(dg::MediaShardHeader::parse(d.data(), d.size(), h));
    CHECK(h.flags & dg::MediaShardHeader::kRandomAccess);
    CHECK(!(h.flags & dg::MediaShardHeader::kParity));
    CHECK(h.picSeq == 77);
    CHECK(h.frameBytes == 1000);
    CHECK(h.totalDataShards == 16);
    CHECK(h.shardBytes == 64);
    CHECK(d.size() == dg::MediaShardHeader::kSize + 64);
  }

  // Shards rebuild the AU (last shard zero-padded).
  std::vector<uint8_t> rebuilt;
  for (const auto& d : pf.dataDatagrams())
    rebuilt.insert(rebuilt.end(), d.begin() + dg::MediaShardHeader::kSize, d.end());
  CHECK(std::equal(au.begin(), au.end(), rebuilt.begin()));

  // Parity: 2 blocks * 2 rows, all parse with PARITY + shardIndex >= k.
  pf.ComputeParity();
  CHECK(pf.parityDatagrams().size() == 2);
  for (size_t bi = 0; bi < 2; bi++) {
    CHECK(pf.parityDatagrams()[bi].size() == 2);
    for (size_t j = 0; j < 2; j++) {
      const auto& d = pf.parityDatagrams()[bi][j];
      dg::MediaShardHeader h;
      CHECK(dg::MediaShardHeader::parse(d.data(), d.size(), h));
      CHECK(h.flags & dg::MediaShardHeader::kParity);
      CHECK(h.shardIndex == 8 + j);
      CHECK(h.blockFirstShard == (bi == 0 ? 0 : 8));
    }
  }
  // ComputeParity is idempotent.
  pf.ComputeParity();
  CHECK(pf.parityDatagrams()[0].size() == 2);

  // Interleave: block0#0, block1#0, block0#1, block1#1, ...
  auto il = pf.interleavedData();
  CHECK(il.size() == 16);
  {
    dg::MediaShardHeader h;
    CHECK(dg::MediaShardHeader::parse(il[0].data(), il[0].size(), h));
    CHECK(h.blockFirstShard == 0 && h.shardIndex == 0);
    CHECK(dg::MediaShardHeader::parse(il[1].data(), il[1].size(), h));
    CHECK(h.blockFirstShard == 8 && h.shardIndex == 0);
    CHECK(dg::MediaShardHeader::parse(il[2].data(), il[2].size(), h));
    CHECK(h.blockFirstShard == 0 && h.shardIndex == 1);
  }

  // Retransmission flags and whole-picture request.
  std::vector<uint16_t> idx = {3};
  auto r = pf.Retransmission(idx);
  CHECK(r.size() == 1);
  CHECK(r[0][1] & dg::MediaShardHeader::kRetransmit);
  CHECK(pf.Retransmission({}).size() == 16);

  // FEC repair end to end: lose shards 1 and 10 of the two blocks; each block
  // keeps k data + all parity, so the block decodes independently.
  for (int blockIdx = 0; blockIdx < 2; blockIdx++) {
    const int base = blockIdx * 8;
    // Decode a block: data shards (erasing one), parity rows 0,1.
    std::vector<std::vector<uint8_t>> d8(8), orig8(8);
    for (int i = 0; i < 8; i++) {
      d8[i].assign(pf.dataDatagrams()[base + i].begin() + dg::MediaShardHeader::kSize,
                   pf.dataDatagrams()[base + i].end());
      orig8[i] = d8[i];
    }
    d8[blockIdx].assign(64, 0);  // erase one data shard per block
    bool present[8] = {true, true, true, true, true, true, true, true};
    present[blockIdx] = false;
    std::vector<const uint8_t*> dp(8);
    for (int i = 0; i < 8; i++) dp[i] = d8[i].data();
    std::vector<const uint8_t*> pp = {
        pf.parityDatagrams()[blockIdx][0].data() + dg::MediaShardHeader::kSize,
        pf.parityDatagrams()[blockIdx][1].data() + dg::MediaShardHeader::kSize};
    int rows[2] = {0, 1};  // parity row indices j within the block
    CHECK(fec::Reconstruct(8, dp.data(), present, rows, pp.data(), 2, 64));
    CHECK(d8[blockIdx] == orig8[blockIdx]);
  }
}

// Embedded depth (GAME_DEPTH_HOST_PLAN.md §3, WIRE.md §4.1/§6.1): the new
// fields round trip, older peers that never set them still decode, and
// DEPTH_VALID rides every shard of a picture including parity and
// retransmissions.
static void TestEmbeddedDepth() {
  // DecoderCaps.embedded_depth (field 8).
  msg::DecoderCaps caps;
  caps.hevc = true;
  caps.embeddedDepth = true;
  caps.maxPixelRate = 1000000000ull;
  pb::Writer cw;
  caps.write(cw);
  msg::DecoderCaps capsOut;
  pb::Reader cr(cw.bytes.data(), cw.bytes.size());
  CHECK(capsOut.read(cr) && capsOut.embeddedDepth && capsOut.hevc);

  // Older peer: no field 8 → stays false.
  msg::DecoderCaps oldCaps;
  oldCaps.hevc = true;
  pb::Writer ow;
  oldCaps.write(ow);
  msg::DecoderCaps oldOut;
  pb::Reader orr(ow.bytes.data(), ow.bytes.size());
  CHECK(oldOut.read(orr) && !oldOut.embeddedDepth);

  // StreamSettings.depth_monitors (field 5, packed repeated uint32).
  msg::Envelope ssIn;
  ssIn.kind = msg::Envelope::Kind::StreamSettings;
  ssIn.streamSettings.audio = true;
  ssIn.streamSettings.depthMonitors = {1, 3};
  auto ssBytes = ssIn.encode();
  msg::Envelope ssOut;
  CHECK(ssOut.decode(ssBytes.data(), ssBytes.size()));
  CHECK(ssOut.streamSettings.audio);
  CHECK(ssOut.streamSettings.depthMonitors == std::vector<uint32_t>({1, 3}));

  // Empty list = no request (an older peer sends the message without it).
  msg::Envelope ssEmpty;
  ssEmpty.kind = msg::Envelope::Kind::StreamSettings;
  ssEmpty.streamSettings.frameRate = 60;
  auto ssE = ssEmpty.encode();
  msg::Envelope ssEOut;
  CHECK(ssEOut.decode(ssE.data(), ssE.size()));
  CHECK(ssEOut.streamSettings.depthMonitors.empty());

  // CodecConfig.embedded_depth (field 10, presence = packing).
  msg::Envelope ccIn;
  ccIn.kind = msg::Envelope::Kind::CodecConfig;
  ccIn.codecConfig.monitorId = 1;
  ccIn.codecConfig.codecGen = 4;
  ccIn.codecConfig.width = 3840;
  ccIn.codecConfig.height = 2312;  // coded size: strip included
  ccIn.codecConfig.hasEmbeddedDepth = true;
  ccIn.codecConfig.embeddedDepth.pictureWidth = 3840;
  ccIn.codecConfig.embeddedDepth.pictureHeight = 2160;
  ccIn.codecConfig.embeddedDepth.stripY = 2176;
  ccIn.codecConfig.embeddedDepth.mapWidth = 960;
  ccIn.codecConfig.embeddedDepth.mapHeight = 540;
  ccIn.codecConfig.embeddedDepth.slices = 4;
  auto ccBytes = ccIn.encode();
  msg::Envelope ccOut;
  CHECK(ccOut.decode(ccBytes.data(), ccBytes.size()));
  CHECK(ccOut.codecConfig.hasEmbeddedDepth);
  CHECK(ccOut.codecConfig.embeddedDepth.pictureHeight == 2160);
  CHECK(ccOut.codecConfig.embeddedDepth.stripY == 2176);
  CHECK(ccOut.codecConfig.embeddedDepth.mapWidth == 960);
  CHECK(ccOut.codecConfig.embeddedDepth.mapHeight == 540);
  CHECK(ccOut.codecConfig.embeddedDepth.slices == 4);
  CHECK(ccOut.codecConfig.embeddedDepth.encoding ==
        msg::DepthEncoding::InverseNearWhite);

  // Without the field there is no layout (older host / no packing).
  msg::Envelope ccPlain;
  ccPlain.kind = msg::Envelope::Kind::CodecConfig;
  ccPlain.codecConfig.codecGen = 5;
  ccPlain.codecConfig.width = 3840;
  ccPlain.codecConfig.height = 2160;
  auto ccP = ccPlain.encode();
  msg::Envelope ccPOut;
  CHECK(ccPOut.decode(ccP.data(), ccP.size()));
  CHECK(!ccPOut.codecConfig.hasEmbeddedDepth);

  // DEPTH_VALID through the packetiser: data, parity and retransmission all
  // carry it (WIRE.md §4.1).
  std::vector<uint8_t> au(4000, 0x5A);
  fec::FrameDescriptor desc;
  desc.codecGen = 2;
  desc.picSeq = 42;
  desc.depthValid = true;
  fec::PacketizedFrame f(desc, au.data(), au.size(), fec::PacketizerConfig::ForDatagramSize(1200));
  f.ComputeParity();
  auto data = f.interleavedData();
  auto parity = f.interleavedParity();
  CHECK(!data.empty() && !parity.empty());
  for (const auto& d : data) {
    dg::MediaShardHeader h;
    CHECK(dg::MediaShardHeader::parse(d.data(), d.size(), h));
    CHECK(h.flags & dg::MediaShardHeader::kDepthValid);
  }
  CHECK(!parity.empty());
  for (const auto& p : parity) {  // interleavedParity(): flat list of parity datagrams
    dg::MediaShardHeader h;
    CHECK(dg::MediaShardHeader::parse(p.data(), p.size(), h));
    CHECK((h.flags & dg::MediaShardHeader::kDepthValid) &&
          (h.flags & dg::MediaShardHeader::kParity));
  }
  auto rtx = f.Retransmission({0});
  CHECK(rtx.size() == 1);
  {
    dg::MediaShardHeader h;
    CHECK(dg::MediaShardHeader::parse(rtx[0].data(), rtx[0].size(), h));
    CHECK((h.flags & dg::MediaShardHeader::kDepthValid) &&
          (h.flags & dg::MediaShardHeader::kRetransmit));
  }

  // A picture without depth never sets the bit.
  desc.depthValid = false;
  fec::PacketizedFrame nd(desc, au.data(), au.size(), fec::PacketizerConfig::ForDatagramSize(1200));
  CHECK(!(nd.dataDatagrams()[0][1] & dg::MediaShardHeader::kDepthValid));
}

static void TestStereoLayout() {
  using msg::StereoLayout;
  for (const auto layout : {StereoLayout::None, StereoLayout::SideBySide,
                            StereoLayout::SideBySideHalf, StereoLayout::TopBottom,
                            StereoLayout::TopBottomHalf}) {
    msg::CodecConfig tx, rx;
    tx.stereoLayout = layout;
    pb::Writer writer;
    tx.write(writer);
    pb::Reader reader(writer.bytes.data(), writer.bytes.size());
    CHECK(rx.read(reader) && rx.stereoLayout == layout && !rx.hasEmbeddedDepth);
  }

  pb::Writer future;
  future.u32(11, 99);
  msg::CodecConfig rx;
  pb::Reader unknown(future.bytes.data(), future.bytes.size());
  CHECK(rx.read(unknown) && rx.stereoLayout == StereoLayout::None);
  const uint8_t halfSbs[] = {0x58, 2};  // field 11, varint, half SBS
  pb::Reader candidate(halfSbs, sizeof(halfSbs));
  CHECK(rx.read(candidate) && rx.stereoLayout == StereoLayout::SideBySideHalf);

  msg::CodecConfig tx;
  tx.stereoLayout = StereoLayout::SideBySide;
  tx.hasEmbeddedDepth = true;
  pb::Writer conflict;
  tx.write(conflict);
  // Even a peer sending both fields cannot enable stereo on a depth picture.
  conflict.u32(11, uint32_t(tx.stereoLayout));
  pb::Reader both(conflict.bytes.data(), conflict.bytes.size());
  CHECK(rx.read(both) && rx.hasEmbeddedDepth && rx.stereoLayout == StereoLayout::None);
}

int main() {
  TestPbBasics();
  TestFraming();
  TestMessages();
  TestDatagrams();
  TestFec();
  TestPacketizer();
  TestEmbeddedDepth();
  TestStereoLayout();
  if (failures) {
    std::printf("wire1_tests: %d FAILURES\n", failures);
    return 1;
  }
  std::printf("wire1_tests: ok\n");
  return 0;
}
