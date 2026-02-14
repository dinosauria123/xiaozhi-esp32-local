#include "ogg_demuxer.h"
#include "esp_log.h"

#define TAG "OggDemuxer"

/// @brief Reset the demuxer
void OggDemuxer::Reset() {
    opus_info_ = {.head_seen = false, .tags_seen = false, .sample_rate = 48000};

    state_ = ParseState::FIND_PAGE;
    ctx_.packet_len = 0;
    ctx_.seg_count = 0;
    ctx_.seg_index = 0;
    ctx_.data_offset = 0;
    ctx_.bytes_needed = 4;  // Need 4 bytes "OggS"
    ctx_.seg_remaining = 0;
    ctx_.body_size = 0;
    ctx_.body_offset = 0;
    ctx_.packet_continued = false;

    // Clear buffer data
    memset(ctx_.header, 0, sizeof(ctx_.header));
    memset(ctx_.seg_table, 0, sizeof(ctx_.seg_table));
    memset(ctx_.packet_buf, 0, sizeof(ctx_.packet_buf));
}

/// @brief Process data block
/// @param data Input data
/// @param size Input data size
/// @return Processed bytes count
size_t OggDemuxer::Process(const uint8_t* data, size_t size) {
    size_t processed = 0;  // Processed bytes count

    while (processed < size) {
        switch (state_) {
            case ParseState::FIND_PAGE: {
                // Find page header "OggS"
                if (ctx_.bytes_needed < 4) {
                    // Handle incomplete "OggS" match (cross data block)
                    size_t to_copy = std::min(size - processed, ctx_.bytes_needed);
                    memcpy(ctx_.header + (4 - ctx_.bytes_needed), data + processed, to_copy);

                    processed += to_copy;
                    ctx_.bytes_needed -= to_copy;

                    if (ctx_.bytes_needed == 0) {
                        // Check if "OggS" matches
                        if (memcmp(ctx_.header, "OggS", 4) == 0) {
                            state_ = ParseState::PARSE_HEADER;
                            ctx_.data_offset = 4;
                            ctx_.bytes_needed =
                                27 - 4;  // Need 23 more bytes to complete page header
                        } else {
                            // Match failed, slide 1 byte to continue matching
                            memmove(ctx_.header, ctx_.header + 1, 3);
                            ctx_.bytes_needed = 1;
                        }
                    } else {
                        // Not enough data, wait for more
                        return processed;
                    }
                } else if (ctx_.bytes_needed == 4) {
                    // Find complete "OggS" in data block
                    bool found = false;
                    size_t i = 0;
                    size_t remaining = size - processed;

                    // Search "OggS"
                    for (; i + 4 <= remaining; i++) {
                        if (memcmp(data + processed + i, "OggS", 4) == 0) {
                            found = true;
                            break;
                        }
                    }

                    if (found) {
                        // Found "OggS", skip bytes already searched
                        processed += i;

                        // Do not record found "OggS", unnecessary
                        // memcpy(ctx_.header, data + processed, 4);
                        processed += 4;

                        state_ = ParseState::PARSE_HEADER;
                        ctx_.data_offset = 4;
                        ctx_.bytes_needed = 27 - 4;  // Need 23 more bytes
                    } else {
                        // Incomplete "OggS" found, save possible partial match
                        size_t partial_len = remaining - i;
                        if (partial_len > 0) {
                            memcpy(ctx_.header, data + processed + i, partial_len);
                            ctx_.bytes_needed = 4 - partial_len;
                            processed += i + partial_len;
                        } else {
                            processed += i;  // All bytes searched
                        }
                        return processed;  // Return processed bytes
                    }
                } else {
                    ESP_LOGE(TAG, "OggDemuxer run in error state: bytes_needed=%zu",
                             ctx_.bytes_needed);
                    Reset();
                    return processed;
                }
                break;
            }

            case ParseState::PARSE_HEADER: {
                size_t available = size - processed;

                if (available < ctx_.bytes_needed) {
                    // Not enough data, copy available part
                    memcpy(ctx_.header + ctx_.data_offset, data + processed, available);

                    ctx_.data_offset += available;
                    ctx_.bytes_needed -= available;
                    processed += available;
                    return processed;  // 等待更多数据
                } else {
                    // Enough data to complete page header
                    size_t to_copy = ctx_.bytes_needed;
                    memcpy(ctx_.header + ctx_.data_offset, data + processed, to_copy);

                    processed += to_copy;
                    ctx_.data_offset += to_copy;
                    ctx_.bytes_needed = 0;

                    // Verify page header
                    if (ctx_.header[4] != 0) {
                        ESP_LOGE(TAG, "Invalid Ogg version: %d", ctx_.header[4]);
                        state_ = ParseState::FIND_PAGE;
                        ctx_.bytes_needed = 4;
                        ctx_.data_offset = 0;
                        break;
                    }

                    ctx_.seg_count = ctx_.header[26];
                    if (ctx_.seg_count > 0 && ctx_.seg_count <= 255) {
                        state_ = ParseState::PARSE_SEGMENTS;
                        ctx_.bytes_needed = ctx_.seg_count;
                        ctx_.data_offset = 0;
                    } else if (ctx_.seg_count == 0) {
                        // No segments, skip to next page
                        state_ = ParseState::FIND_PAGE;
                        ctx_.bytes_needed = 4;
                        ctx_.data_offset = 0;
                    } else {
                        ESP_LOGE(TAG, "Invalid segment count: %u", ctx_.seg_count);
                        state_ = ParseState::FIND_PAGE;
                        ctx_.bytes_needed = 4;
                        ctx_.data_offset = 0;
                    }
                }
                break;
            }

            case ParseState::PARSE_SEGMENTS: {
                size_t available = size - processed;

                if (available < ctx_.bytes_needed) {
                    memcpy(ctx_.seg_table + ctx_.data_offset, data + processed, available);

                    ctx_.data_offset += available;
                    ctx_.bytes_needed -= available;
                    processed += available;
                    return processed;  // 等待更多数据
                } else {
                    size_t to_copy = ctx_.bytes_needed;
                    memcpy(ctx_.seg_table + ctx_.data_offset, data + processed, to_copy);

                    processed += to_copy;
                    ctx_.data_offset += to_copy;
                    ctx_.bytes_needed = 0;

                    state_ = ParseState::PARSE_DATA;
                    ctx_.seg_index = 0;
                    ctx_.data_offset = 0;

                    // Calculate total body size
                    ctx_.body_size = 0;
                    for (size_t i = 0; i < ctx_.seg_count; ++i) {
                        ctx_.body_size += ctx_.seg_table[i];
                    }
                    ctx_.body_offset = 0;
                    ctx_.seg_remaining = 0;
                }
                break;
            }

            case ParseState::PARSE_DATA: {
                while (ctx_.seg_index < ctx_.seg_count && processed < size) {
                    uint8_t seg_len = ctx_.seg_table[ctx_.seg_index];

                    // Check if segment data is partially read
                    if (ctx_.seg_remaining > 0) {
                        seg_len = ctx_.seg_remaining;
                    } else {
                        ctx_.seg_remaining = seg_len;
                    }

                    // Check if buffer is sufficient
                    if (ctx_.packet_len + seg_len > sizeof(ctx_.packet_buf)) {
                        ESP_LOGE(TAG, "Packet buffer overflow: %zu + %u > %zu", ctx_.packet_len,
                                 seg_len, sizeof(ctx_.packet_buf));
                        state_ = ParseState::FIND_PAGE;
                        ctx_.packet_len = 0;
                        ctx_.packet_continued = false;
                        ctx_.seg_remaining = 0;
                        ctx_.bytes_needed = 4;
                        return processed;
                    }

                    // Copy data
                    size_t to_copy = std::min(size - processed, (size_t)seg_len);
                    memcpy(ctx_.packet_buf + ctx_.packet_len, data + processed, to_copy);

                    processed += to_copy;
                    ctx_.packet_len += to_copy;
                    ctx_.body_offset += to_copy;
                    ctx_.seg_remaining -= to_copy;

                    // Check if segment is complete
                    if (ctx_.seg_remaining > 0) {
                        // Segment incomplete, wait for more data
                        return processed;
                    }

                    // Segment complete
                    bool seg_continued = (ctx_.seg_table[ctx_.seg_index] == 255);

                    if (!seg_continued) {
                        // Packet end
                        if (ctx_.packet_len) {
                            if (!opus_info_.head_seen) {
                                if (ctx_.packet_len >= 8 &&
                                    memcmp(ctx_.packet_buf, "OpusHead", 8) == 0) {
                                    opus_info_.head_seen = true;
                                    if (ctx_.packet_len >= 19) {
                                        opus_info_.sample_rate = ctx_.packet_buf[12] |
                                                                 (ctx_.packet_buf[13] << 8) |
                                                                 (ctx_.packet_buf[14] << 16) |
                                                                 (ctx_.packet_buf[15] << 24);
                                        ESP_LOGI(TAG, "OpusHead found, sample_rate=%d",
                                                 opus_info_.sample_rate);
                                    }
                                    ctx_.packet_len = 0;
                                    ctx_.packet_continued = false;
                                    ctx_.seg_index++;
                                    ctx_.seg_remaining = 0;
                                    continue;
                                }
                            }
                            if (!opus_info_.tags_seen) {
                                if (ctx_.packet_len >= 8 &&
                                    memcmp(ctx_.packet_buf, "OpusTags", 8) == 0) {
                                    opus_info_.tags_seen = true;
                                    ESP_LOGI(TAG, "OpusTags found.");
                                    ctx_.packet_len = 0;
                                    ctx_.packet_continued = false;
                                    ctx_.seg_index++;
                                    ctx_.seg_remaining = 0;
                                    continue;
                                }
                            }
                            if (opus_info_.head_seen && opus_info_.tags_seen) {
                                if (on_demuxer_finished_) {
                                    on_demuxer_finished_(ctx_.packet_buf, opus_info_.sample_rate,
                                                         ctx_.packet_len);
                                }
                            } else {
                                ESP_LOGW(TAG,
                                         "Current Ogg container did not parse OpusHead/OpusTags, "
                                         "discarding");
                            }
                        }
                        ctx_.packet_len = 0;
                        ctx_.packet_continued = false;
                    } else {
                        ctx_.packet_continued = true;
                    }

                    ctx_.seg_index++;
                    ctx_.seg_remaining = 0;
                }

                if (ctx_.seg_index == ctx_.seg_count) {
                    // Check if all body data has been read
                    if (ctx_.body_offset < ctx_.body_size) {
                        ESP_LOGW(TAG, "Body incomplete: %zu/%zu", ctx_.body_offset, ctx_.body_size);
                    }

                    // If packet spans pages, keep packet_len and packet_continued
                    if (!ctx_.packet_continued) {
                        ctx_.packet_len = 0;
                    }

                    // Enter next page
                    state_ = ParseState::FIND_PAGE;
                    ctx_.bytes_needed = 4;
                    ctx_.data_offset = 0;
                }
                break;
            }
        }
    }

    return processed;
}
