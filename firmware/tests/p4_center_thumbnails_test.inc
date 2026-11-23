/* Runs the production decoder/cache/blit against the linked catalog blob. */
static bool draw_test_card(pet_p4_miniapp_catalog_entry_t *entry, unsigned slot) {
  bool allow = true;
  return center_draw_thumbnail(entry, slot, 12, 8, &allow) == CENTER_THUMB_DRAWN;
}

/* Reproduce the installed on-disk format under an isolated temporary mount. */
static void fixture_thumbnail(pet_p4_miniapp_catalog_entry_t *item, uint8_t color, size_t length) {
  uint8_t bytes[CENTER_THUMB_BYTES+1] = {0};
  bytes[2] = color; bytes[3] = 0x12; bytes[4] = 0xcd; bytes[5] = 0xab;
  memset(bytes+32, 0x12, sizeof(bytes)-32);
  item->center_thumb_checksum = center_thumb_hash(bytes);
  if (length) {
    char path[40];
    center_thumb_path(item->center_title_slot, item->center_thumb_generation, false, path, sizeof(path));
    FILE *file = fopen(path+8, "wb");
    assert(file && fwrite(bytes, 1, length, file) == length && !fclose(file));
  }
}

static void test_thumbnail_cache(void) {
  pet_p4_miniapp_catalog_entry_t item = {0};
  strcpy(item.widget_id, "city-radio");
  fail_allocation = true;
  center_thumbnails_prepare();
  assert(!draw_test_card(&item, 0) && !outstanding);
  center_thumbnails_release();
  fail_allocation = false;
  center_thumbnails_prepare();
  assert(sizeof(*center_thumb_page) < 27*1024 && outstanding == 1 && inflations == 0);

  /* Each visible slot decodes mapped art on the first turn. A repeated hit never unpacks or reads. */
  bool allow = true;
  assert(center_draw_thumbnail(&item, 0, 12, 8, &allow) == CENTER_THUMB_DRAWN && allow);
  assert(center_draw_thumbnail(&item, 1, 12, 8, &allow) == CENTER_THUMB_DRAWN);
  assert(draw_test_card(&item, 0) && inflations == 2 && !package_reads);
  for (unsigned i = 0; i < CENTER_ART_COUNT; ++i) {
    const size_t offset = center_art_catalog[i].offset, length = center_art_catalog[i].length;
    if (!center_art_catalog[i].art) continue;
    assert(length && offset + length <= sizeof(center_thumbs_z));
    strcpy(item.widget_id, center_art_catalog[i].id);
    unsigned slot = i % PET_CENTER_PAGE_SIZE;
    assert(draw_test_card(&item, slot));
    uint8_t expected[CENTER_THUMB_BYTES];
    uLongf size = sizeof(expected);
    assert(uncompress(expected, &size, center_thumbs_z + offset, length) == Z_OK && size == sizeof(expected));
    for (int y = 0; y < 48; ++y) for (int x = 0; x < 64; ++x) {
      int dx = x<7 ? 7-x : x>56 ? x-56 : 0, dy = y<7 ? 7-y : y>40 ? y-40 : 0;
      if (dx*dx+dy*dy>49) continue;
      int k = 64*y+x, pair = expected[32+k/2], index = k%2 ? pair&15 : pair>>4;
      assert(g_framebuffer[(8+y)*640+12+x] == (expected[2*index] | expected[2*index+1]<<8));
    }
  }
  /* A failed member doesn't poison its neighbour; missing art leaves the card's icon path. */
  strcpy(item.widget_id, center_art_catalog[0].id);
  size_t offset = center_art_catalog[0].offset;
  center_thumbs_z[offset] ^= 255;
  ++item.center_thumb_generation;
  assert(!draw_test_card(&item, 0));
  strcpy(item.widget_id, center_art_catalog[1].id);
  assert(draw_test_card(&item, 0));
  center_thumbs_z[offset] ^= 255;
  strcpy(item.widget_id, "legacy-without-cover");
  assert(!draw_test_card(&item, 0));

  /* A pending installed cover must not be replaced with different built-in art. */
  strcpy(item.widget_id, center_art_catalog[0].id);
  fixture_thumbnail(&item, 0x34, 0);
  assert(center_draw_thumbnail(&item, 0, 12, 8, &allow) == CENTER_THUMB_PENDING && package_reads == 1);
  fixture_thumbnail(&item, 0x34, CENTER_THUMB_BYTES);
  assert(!draw_test_card(&item, 0) && package_reads == 1);
  clock_us += 1000000;
  g_framebuffer[8*640+12] = 0xf00d;
  assert(draw_test_card(&item, 0) && package_reads == 2);
  assert(g_framebuffer[8*640+12] == 0xf00d);
  assert(g_framebuffer[16*640+12] == 0x1234 && g_framebuffer[16*640+13] == 0xabcd);
  unsigned decoded = inflations;
  strcpy(item.widget_id, "third-party");
  fixture_thumbnail(&item, 0x56, CENTER_THUMB_BYTES);
  allow = false;
  assert(center_draw_thumbnail(&item, 0, 12, 8, &allow) == CENTER_THUMB_PENDING); /* Never show the old slot. */
  assert(draw_test_card(&item, 0) && g_framebuffer[16*640+12] == 0x1256);
  ++item.center_thumb_generation;
  fixture_thumbnail(&item, 0x56, CENTER_THUMB_BYTES);
  assert(draw_test_card(&item, 0) && package_reads == 4);
  ++item.center_title_slot;
  fixture_thumbnail(&item, 0x56, CENTER_THUMB_BYTES);
  assert(draw_test_card(&item, 0) && package_reads == 5);
  fixture_thumbnail(&item, 0x57, CENTER_THUMB_BYTES);
  assert(draw_test_card(&item, 0) && package_reads == 6);
  assert(g_framebuffer[16*640+12] == 0x1257 && inflations == decoded);
  uint8_t probe[CENTER_THUMB_BYTES];
  fixture_thumbnail(&item, 0x58, CENTER_THUMB_BYTES-1);
  assert(!pet_p4_miniapp_center_read_thumb(&item, probe, sizeof(probe)));
  fixture_thumbnail(&item, 0x58, CENTER_THUMB_BYTES+1);
  assert(!pet_p4_miniapp_center_read_thumb(&item, probe, sizeof(probe)));
  fixture_thumbnail(&item, 0x58, CENTER_THUMB_BYTES);
  fixture_thumbnail(&item, 0x59, 0); /* same path and size, wrong expected hash */
  for (unsigned i = 0; i < 10; ++i) {
    clock_us += 1000000;
    assert(!draw_test_card(&item, 0));
  }
  assert(package_reads == 11);
  /* One static page, no input or catalog updates. Hold audio for six seconds.
   * No card shows substitute art while its installed cover is waiting. */
  for (unsigned slot = 0; slot < PET_CENTER_PAGE_SIZE; ++slot) {
    item.center_title_slot = slot;
    item.center_thumb_generation = 0;
    fixture_thumbnail(&item, (uint8_t)(0x60+slot), CENTER_THUMB_BYTES);
    test_package_checksums[slot] = item.center_thumb_checksum;
  }
  count = PET_CENTER_PAGE_SIZE;
  selected = 0;
  storage_busy = true;
  assert(center_prepare_labels());
  decoded = inflations;
  uint64_t last_render_ms = clock_us/1000;
  unsigned before = package_reads;
  for (int frame = 0; frame < 120; ++frame) {
    clock_us += 50000;
    assert(static_center_frame_due(clock_us/1000, last_render_ms));
    last_render_ms = clock_us/1000;
    fallback_tiles = 0;
    render_component_center_page(false);
    assert(package_reads == before && fallback_tiles == 0);
  }
  assert(inflations == decoded);
  storage_busy = false;
  for (int frame = 0; frame < 12; ++frame) {
    clock_us += 50000;
    assert(static_center_frame_due(clock_us/1000, last_render_ms));
    last_render_ms = clock_us/1000;
    unsigned previous = package_reads;
    render_component_center_page(false);
    assert(package_reads-previous <= 1);
  }
  assert(package_reads == before+PET_CENTER_PAGE_SIZE);
  for (int slot = 0; slot < PET_CENTER_PAGE_SIZE; ++slot) {
    int x = PET_CENTER_LEFT + slot%2 * PET_CENTER_STEP_X + 18;
    int y = PET_CENTER_TOP + slot/2 * PET_CENTER_STEP_Y + 14;
    assert(g_framebuffer[(y+8)*640+x] == 0x1260+slot);
  }
  /* Keep the icon area neutral during retries. Only the final decision
   * selects built-in art or the legacy icon, depending on the component. */
  for (unsigned builtin = 0; builtin < 2; ++builtin) {
    strcpy(item.widget_id, builtin ? center_art_catalog[0].id : "third-party");
    item.center_title_slot = 31;
    fixture_thumbnail(&item, 0x70, 0);
    for (unsigned attempt = 1; attempt <= CENTER_THUMB_MAX_ATTEMPTS; ++attempt) {
      clock_us += 1000000;
      allow = true;
      center_thumb_state_t state = center_draw_thumbnail(&item, 0, 12, 8, &allow);
      assert(!allow);
      if (attempt < CENTER_THUMB_MAX_ATTEMPTS) assert(state == CENTER_THUMB_PENDING);
      else assert(state == (builtin ? CENTER_THUMB_DRAWN : CENTER_THUMB_MISSING));
    }
  }
  memset(test_package_checksums, 0, sizeof(test_package_checksums));
  center_thumbnails_release();
  center_thumbnails_release();
  assert(!outstanding);
}
