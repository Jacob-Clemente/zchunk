/*
 * Copyright 2018 Jonathan Dieter <jdieter@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <curl/curl.h>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#include <zck.h>

#include "zck_private.h"

#define VALIDATE(f)     if(!f) { \
                            zck_log(ZCK_LOG_ERROR, "zckDL not allocated\n"); \
                            return False; \
                        }

int zck_dl_write_zero(zckCtx *tgt, zckIndex *tgt_idx) {
    char buf[BUF_SIZE] = {0};
    size_t tgt_data_offset = tgt->preindex_size + tgt->comp_index_size;
    size_t to_read = tgt_idx->length;
    if(!zck_seek(tgt->fd, tgt_data_offset + tgt_idx->start, SEEK_SET))
        return False;
    while(to_read > 0) {
        int rb = BUF_SIZE;
        if(rb > to_read)
            rb = to_read;
        if(!zck_write(tgt->fd, buf, rb))
            return False;
        to_read -= rb;
    }
    return True;
}

int zck_dl_write(zckDL *dl, const char *at, size_t length) {
    if(dl->write_in_chunk < length)
        length = dl->write_in_chunk;
    if(!zck_write(dl->dst_fd, at, length))
        return -1;
    dl->write_in_chunk -= length;
    return length;
}

int zck_dl_md_write(zckDL *dl, const char *at, size_t length) {
    int wb = 0;
    if(dl->write_in_chunk > 0) {
        wb = zck_dl_write(dl, at, length);
        if(!zck_hash_update(dl->chunk_hash, at, wb))
            return 0;
        if(wb < 0)
            return 0;
        zck_log(ZCK_LOG_DEBUG, "Writing %lu bytes\n", wb);
        dl->dl_chunk_data += wb;
    }
    return wb;
}

int zck_dl_write_chunk(zckDL *dl) {
    if(dl->chunk_hash == NULL) {
        zck_log(ZCK_LOG_ERROR, "Chunk hash not initialized\n");
        return False;
    }
    char *digest = zck_hash_finalize(dl->chunk_hash);
    free(dl->chunk_hash);
    if(memcmp(digest, dl->tgt_check->digest, dl->tgt_check->digest_size) != 0) {
        zck_log(ZCK_LOG_WARNING,
                "Downloaded chunk failed hash check\n");
        if(!zck_dl_write_zero(dl->zck, dl->tgt_check))
            return False;
    } else {
        dl->tgt_check->finished = True;
    }
    dl->tgt_check = NULL;
    dl->chunk_hash = NULL;
    free(digest);
    return True;
}

int zck_dl_multidata_cb(zckDL *dl, const char *at, size_t length) {
    if(dl == NULL || dl->info.index.first == NULL) {
        zck_log(ZCK_LOG_ERROR, "zckDL index not initialized\n");
        return 0;
    }
    if(dl->zck == NULL || dl->zck->index.first == NULL) {
        zck_log(ZCK_LOG_ERROR, "zckCtx index not initialized\n");
        return 0;
    }
    int wb = zck_dl_md_write(dl, at, length);
    if(dl->write_in_chunk == 0) {
        /* Check whether we just finished downloading a chunk and verify it */
        if(dl->tgt_check && !zck_dl_write_chunk(dl))
            return False;
        zckIndex *idx = dl->info.index.first;
        while(idx) {
            if(dl->dl_chunk_data == idx->start) {
                zckIndex *tgt_idx = dl->zck->index.first;
                while(tgt_idx) {
                    if(tgt_idx->finished)
                        tgt_idx = tgt_idx->next;
                    if(idx->length == tgt_idx->length &&
                       memcmp(idx->digest, tgt_idx->digest,
                              idx->digest_size) == 0) {
                        dl->tgt_check = tgt_idx;
                        dl->chunk_hash = zmalloc(sizeof(zckHash));
                        if(!zck_hash_init(dl->chunk_hash,
                                          &(dl->zck->chunk_hash_type)))
                            return 0;
                        dl->write_in_chunk = idx->length;
                        size_t offset = dl->zck->preindex_size +
                                        dl->zck->comp_index_size;
                        if(!zck_seek(dl->dst_fd, offset + tgt_idx->start,
                           SEEK_SET))
                            return 0;
                        idx = NULL;
                        tgt_idx = NULL;
                    } else {
                        tgt_idx = tgt_idx->next;
                    }
                }
            }
            if(idx)
                idx = idx->next;
        }
    }
    int wb2 = 0;
    if(dl->write_in_chunk > 0 && wb < length) {
        wb2 = zck_dl_multidata_cb(dl, at+wb, length-wb);
        if(wb2 == 0)
            return 0;
    }
    return wb + wb2;
}

zckDL *zck_dl_init() {
    zckDL *dl = zmalloc(sizeof(zckDL));
    if(!dl) {
        zck_log(ZCK_LOG_ERROR, "Unable to allocate %lu bytes for zckDL\n",
                sizeof(zckDL));
        return NULL;
    }
    dl->priv = zmalloc(sizeof(zckDLPriv));
    if(!dl->priv) {
        zck_log(ZCK_LOG_ERROR, "Unable to allocate %lu bytes for dl->priv\n",
                sizeof(zckDL));
        return NULL;
    }
    dl->priv->mp = zmalloc(sizeof(zckMP));
    if(!dl->priv->mp) {
        zck_log(ZCK_LOG_ERROR,
                "Unable to allocate %lu bytes for dl->priv->mp\n",
                sizeof(zckMP));
        return NULL;
    }
    dl->priv->curl_ctx = curl_easy_init();
    if(!dl->priv->curl_ctx) {
        zck_log(ZCK_LOG_ERROR, "Unable to allocate %lu bytes for dl->curl_ctx\n",
                sizeof(CURL));
        return NULL;
    }
    return dl;
}

void zck_dl_free(zckDL *dl) {
    if(!dl)
        return;
    if(dl->priv) {
        if(dl->priv->mp) {
            if(dl->priv->mp->buffer)
                free(dl->priv->mp->buffer);
            free(dl->priv->mp);
        }
        curl_easy_cleanup(dl->priv->curl_ctx);
        free(dl->priv);
    }
    if(dl->info.first)
        zck_range_close(&(dl->info));
    if(dl->boundary)
        free(dl->boundary);
    free(dl);
}

char *zck_dl_get_range_char(unsigned int start, unsigned int end) {
    zckRange r = {0};
    zckRange *range = &r;

    r.start = start;
    r.end = end;
    char *range_header = zck_range_get_char(&range, 2);
    return range_header;
}

static size_t extract_multipart(char *b, size_t l, void *dl_v) {
    if(dl_v == NULL)
        return 0;
    zckDL *dl = (zckDL*)dl_v;
    if(dl->priv == NULL || dl->priv->mp == NULL)
        return 0;
    zckMP *mp = dl->priv->mp;
    char *buf = b;
    int alloc_buf = False;

    if(mp->buffer) {
        buf = realloc(mp->buffer, mp->buffer_len + l);
        if(buf == NULL) {
            zck_log(ZCK_LOG_ERROR, "Unable to reallocate %lu bytes for zckDL\n",
                    mp->buffer_len + l);
            return 0;
        }
        memcpy(buf + mp->buffer_len, b, l);
        l = mp->buffer_len + l;
        mp->buffer = NULL;  // No need to free, buf holds realloc'd buffer
        mp->buffer_len = 0;
        alloc_buf = True;
    }
    char *header_start = buf;
    char *i = buf;
    while(i) {
        char *end = buf + l;
        if(mp->state != 0) {
            if(i >= end)
                break;
            size_t size = end - i;
            if(mp->length <= size) {
                size = mp->length;
                mp->length = 0;
                mp->state = 0;
                header_start = i + size;
            } else {
                mp->length -= size;
            }
            if(zck_dl_multidata_cb(dl, i, size) != size)
                return 0;
            i += size;
            continue;
        }
        if(i >= end) {
            size_t size = buf + l - header_start;
            if(size > 0) {
                mp->buffer = malloc(size);
                memcpy(mp->buffer, header_start, size);
                mp->buffer_len = size;
            }
            break;
        }

        if(i + 4 + strlen(dl->boundary) + 4 > end) {
            i += 4 + strlen(dl->boundary) + 4;
            continue;
        }
        if(memcmp(i, "\r\n--", 4) != 0) {
            zck_log(ZCK_LOG_ERROR, "Multipart boundary header invalid\n");
            l = 0;
            goto end;
        }
        i += 4;
        if(memcmp(i, dl->boundary, strlen(dl->boundary)) != 0) {
            zck_log(ZCK_LOG_ERROR, "Multipart boundary not matched\n");
            l = 0;
            goto end;
        }
        i += strlen(dl->boundary);
        if(memcmp(i, "--\r\n", 4) == 0) {
            if(i + 4 != end)
                zck_log(ZCK_LOG_WARNING,
                        "Multipart data end with %lu bytes still remaining\n",
                        end - i - 4);
            else
                zck_log(ZCK_LOG_DEBUG, "Multipart data end\n");
            goto end;
        }
        if(i + 15 > end) {
            i += 15;
            continue;
        }
        if(memcmp(i, "\r\nContent-type:", 15) != 0) {
            zck_log(ZCK_LOG_ERROR, "Multipart type header invalid\n");
            l = 0;
            goto end;
        }
        i += 15;
        while(True) {
            if(i + 2 > end || memcmp(i, "\r\n", 2) == 0) {
                i += 2;
                break;
            }
            i++;
        }
        if(i + 21 > end) {
            i += 21;
            continue;
        }
        if(memcmp(i, "Content-range: bytes ", 21) != 0) {
            zck_log(ZCK_LOG_ERROR, "Multipart range header invalid\n");
            l = 0;
            goto end;
        }
        i += 21;
        size_t rstart = 0;
        size_t rend = 0;
        size_t good = False;
        while(True) {
            if(i + 1 > end || memcmp(i, "-", 1) == 0) {
                i++;
                break;
            }
            rstart = rstart*10 + (size_t)(i[0] - 48);
            good = True;
            i++;
        }
        if(i > end)
            continue;
        if(!good) {
            zck_log(ZCK_LOG_ERROR, "Multipart start range missing\n");
            l = 0;
            goto end;
        }
        good = False;
        while(True) {
            if(i + 1 > end || memcmp(i, "/", 1) == 0) {
                i++;
                break;
            }
            rend = rend*10 + (size_t)(i[0] - 48);
            good = True;
            i++;
        }
        if(i > end)
            continue;
        if(!good) {
            zck_log(ZCK_LOG_ERROR, "Multipart end range missing\n");
            l = 0;
            goto end;
        }
        while(True) {
            if(i + 4 >= end || memcmp(i, "\r\n\r\n", 4) == 0) {
                i += 4;
                break;
            }
            i++;
        }
        zck_log(ZCK_LOG_DEBUG, "Download range: %lu-%lu\n", rstart, rend);
        mp->length = rend-rstart+1;
        mp->state = 1;
    }
end:
    if(alloc_buf)
        free(buf);
    return l;
}

static size_t get_header(char *b, size_t l, size_t c, void *dl_v) {
    if(dl_v == NULL)
        return 0;
    zckDL *dl = (zckDL*)dl_v;

    if(l*c < 14 || strncmp("Content-Type:", b, 13) != 0)
        return l*c;

    size_t size = l*c;
    /* Null terminate buffer */
    b += 13;
    size -= 13;
    while(size > 2 && (b[size-1] == '\n' || b[size-1] == '\r'))
        size--;
    if(size <= 2)
        return l*c;
    char *buf = zmalloc(size+1);
    if(buf == NULL) {
        zck_log(ZCK_LOG_ERROR, "Unable to allocate %lu bytes for header\n",
                size+1);
        return 0;
    }
    buf[size] = '\0';
    memcpy(buf, b, size);
    char *loc = buf;
    while(loc[0] == ' ') {
        loc++;
        size--;
        if(size <= 0)
            goto end;
    }
    if(size < 22 || strncmp("multipart/byteranges;", loc, 21) != 0)
        goto end;
    loc += 21;
    size -= 21;
    while(loc[0] == ' ') {
        loc++;
        size--;
        if(size <= 0)
            goto end;
    }
    if(size < 10 || strncmp("boundary=", loc, 9) != 0)
        goto end;
    loc += 9;
    size -= 9;
    while(loc[0] == ' ') {
        loc++;
        size--;
        if(size <= 0)
            goto end;
    }
    char *boundary = zmalloc(size+1);
    memcpy(boundary, loc, size+1);
    zck_log(ZCK_LOG_DEBUG, "Multipart boundary: %s\n", boundary);
    dl->boundary = boundary;
end:
    free(buf);
    return l*c;
}
static size_t write_data(void *ptr, size_t l, size_t c, void *dl_v) {
    if(dl_v == NULL)
        return 0;
    zckDL *dl = (zckDL*)dl_v;
    size_t wb = 0;
    dl->dl += l*c;
    if(dl->boundary != NULL) {
        int retval = extract_multipart(ptr, l*c, dl_v);
        if(retval == 0)
            wb = 0;
        else
            wb = l*c;
    } else {
        wb = write(dl->dst_fd, ptr, l*c);
    }
    return wb;
}

int zck_dl_write_and_verify(zckRangeInfo *info, zckCtx *src, zckCtx *tgt,
                            zckIndex *src_idx, zckIndex *tgt_idx) {
    static char buf[BUF_SIZE] = {0};

    size_t src_data_offset = src->preindex_size + src->comp_index_size;
    size_t tgt_data_offset = tgt->preindex_size + tgt->comp_index_size;
    size_t to_read = src_idx->length;
    if(!zck_seek(src->fd, src_data_offset + src_idx->start, SEEK_SET))
        return False;
    if(!zck_seek(tgt->fd, tgt_data_offset + tgt_idx->start, SEEK_SET))
        return False;
    zckHash check_hash = {0};
    if(!zck_hash_init(&check_hash, &(src->chunk_hash_type)))
        return False;
    while(to_read > 0) {
        int rb = BUF_SIZE;
        if(rb > to_read)
            rb = to_read;
        if(!zck_read(src->fd, buf, rb))
            return False;
        if(!zck_hash_update(&check_hash, buf, rb))
            return False;
        if(!zck_write(tgt->fd, buf, rb))
            return False;
        to_read -= rb;
    }
    char *digest = zck_hash_finalize(&check_hash);
    /* If chunk is invalid, overwrite with zeros and add to download range */
    if(memcmp(digest, src_idx->digest, src_idx->digest_size) != 0) {
        zck_log(ZCK_LOG_WARNING, "Source hash: %s\n",
                zck_hash_get_printable(src_idx->digest, &(src->chunk_hash_type)));
        zck_log(ZCK_LOG_WARNING, "Target hash: %s\n",
                zck_hash_get_printable(digest, &(src->chunk_hash_type)));
        if(!zck_dl_write_zero(tgt, tgt_idx))
            return False;
        if(!zck_range_add(info, tgt_idx, tgt))
            return False;
    } else {
        tgt_idx->finished = True;
        zck_log(ZCK_LOG_DEBUG, "Writing %lu bytes at %lu\n", tgt_idx->length,
                tgt_idx->start);
    }
    free(digest);
    return True;
}

int zck_dl_copy_src_chunks(zckRangeInfo *info, zckCtx *src, zckCtx *tgt) {
    zckIndexInfo *tgt_info = zck_get_index(tgt);
    zckIndexInfo *src_info = zck_get_index(src);
    zckIndex *tgt_idx = tgt_info->first;
    zckIndex *src_idx = src_info->first;
    while(tgt_idx) {
        int found = False;
        src_idx = src_info->first;

        while(src_idx) {
            if(tgt_idx->length == src_idx->length &&
               memcmp(tgt_idx->digest, src_idx->digest,
                      zck_get_chunk_digest_size(tgt)) == 0) {
                found = True;
                break;
            }
            src_idx = src_idx->next;
        }
        /* Write out found chunk, then verify that it's valid */
        if(found && !zck_dl_write_and_verify(info, src, tgt, src_idx, tgt_idx))
            return False;
        if(!found && !zck_range_add(info, tgt_idx, tgt))
            return False;

        tgt_idx = tgt_idx->next;
    }
    return True;
}

int zck_dl_range(zckDL *dl, char *url) {
    if(dl == NULL || dl->priv == NULL || dl->info.first == NULL) {
        zck_log(ZCK_LOG_ERROR, "Struct not defined\n");
        return False;
    }
    if(dl->priv->parser_started) {
        zck_log(ZCK_LOG_ERROR, "Multipart parser already started\n");
        return False;
    }
    if(dl->info.segments == 0)
        dl->info.segments = 1;

    char **ra = calloc(sizeof(char*), dl->info.segments);
    if(!zck_range_get_array(&(dl->info), ra)) {
        free(ra);
        return False;
    }
    CURLcode res;

    for(int i=0; i<dl->info.segments; i++) {
        struct curl_slist *header = NULL;
        header = curl_slist_append(header, ra[i]);
        curl_easy_setopt(dl->priv->curl_ctx, CURLOPT_URL, url);
        curl_easy_setopt(dl->priv->curl_ctx, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(dl->priv->curl_ctx, CURLOPT_HEADERFUNCTION, get_header);
        curl_easy_setopt(dl->priv->curl_ctx, CURLOPT_HEADERDATA, dl);
        curl_easy_setopt(dl->priv->curl_ctx, CURLOPT_WRITEFUNCTION, write_data);
        curl_easy_setopt(dl->priv->curl_ctx, CURLOPT_WRITEDATA, dl);
        curl_easy_setopt(dl->priv->curl_ctx, CURLOPT_HTTPHEADER, header);
        res = curl_easy_perform(dl->priv->curl_ctx);
        curl_slist_free_all(header);
        free(ra[i]);

        if(res != CURLE_OK) {
            zck_log(ZCK_LOG_ERROR, "Download failed: %s\n",
                    curl_easy_strerror(res));
            return False;
        }
        long code;
        curl_easy_getinfo (dl->priv->curl_ctx, CURLINFO_RESPONSE_CODE, &code);
        if (code != 206 && code != 200) {
            zck_log(ZCK_LOG_ERROR, "HTTP Error: %li when download %s\n", code,
                    url);
            return False;
        }
    }
    free(ra);
    return True;
}

int zck_dl_bytes(zckDL *dl, char *url, size_t bytes, size_t start,
                 size_t *buffer_len) {
    if(dl == NULL) {
        zck_log(ZCK_LOG_ERROR, "zckDL not initialized\n");
        return False;
    }
    if(start + bytes > *buffer_len) {
        zckIndex idx = {0};

        zck_log(ZCK_LOG_DEBUG, "Seeking to end of temporary file\n");
        if(lseek(dl->dst_fd, 0, SEEK_END) == -1) {
            zck_log(ZCK_LOG_ERROR, "Seek to end of temporary file failed: %s\n",
                    strerror(errno));
            return False;
        }
        zck_log(ZCK_LOG_DEBUG, "Downloading %lu bytes at position %lu\n", start+bytes-*buffer_len, *buffer_len);
        idx.start = *buffer_len;
        idx.length = start+bytes-*buffer_len;
        zck_range_close(&(dl->info));
        zck_range_add(&(dl->info), &idx, NULL);
        if(!zck_dl_range(dl, url))
            return False;
        zck_range_close(&(dl->info));
        *buffer_len = start+bytes;
        zck_log(ZCK_LOG_DEBUG, "Seeking to position %lu\n", start);
        if(lseek(dl->dst_fd, start, SEEK_SET) == -1) {
            zck_log(ZCK_LOG_ERROR,
                    "Seek to byte %lu of temporary file failed: %s\n", start,
                    strerror(errno));
            return False;
        }
    }
    return True;
}

int zck_zero_bytes(zckDL *dl, size_t bytes, size_t start, size_t *buffer_len) {
    char buf[BUF_SIZE] = {0};
    if(start + bytes > *buffer_len) {
        zck_log(ZCK_LOG_DEBUG, "Seeking to end of temporary file\n");
        if(lseek(dl->dst_fd, 0, SEEK_END) == -1) {
            zck_log(ZCK_LOG_ERROR, "Seek to end of temporary file failed: %s\n",
                    strerror(errno));
            return False;
        }
        size_t write = *buffer_len;
        while(write < start + bytes) {
            size_t wb = BUF_SIZE;
            if(write + wb > start + bytes)
                wb = (start + bytes) - write;
            if(!zck_write(dl->dst_fd, buf, wb))
                return False;
            write += wb;
        }
        zck_log(ZCK_LOG_DEBUG, "Wrote %lu zeros at position %lu\n", start+bytes-*buffer_len, *buffer_len);
        *buffer_len = start+bytes;
        zck_log(ZCK_LOG_DEBUG, "Seeking to position %lu\n", start);
        if(lseek(dl->dst_fd, start, SEEK_SET) == -1) {
            zck_log(ZCK_LOG_ERROR,
                    "Seek to byte %lu of temporary file failed: %s\n", start,
                    strerror(errno));
            return False;
        }
    }
    return True;
}

int zck_dl_get_header(zckCtx *zck, zckDL *dl, char *url) {
    size_t buffer_len = 0;
    size_t start = 0;
    if(zck == NULL) {
        zck_log(ZCK_LOG_ERROR, "zckCtx not initialized\n");
        return False;
    }
    if(dl == NULL) {
        zck_log(ZCK_LOG_ERROR, "zckDL not initialized\n");
        return False;
    }
    zck->fd = dl->dst_fd;
    if(!zck_dl_bytes(dl, url, 100, start, &buffer_len))
        return False;
    if(!zck_read_initial(zck, dl->dst_fd))
        return False;
    start += 6;
    if(!zck_dl_bytes(dl, url, zck->hash_type.digest_size+9, start,
                     &buffer_len))
        return False;
    if(!zck_read_index_hash(zck, dl->dst_fd))
        return False;
    start += zck->hash_type.digest_size;

    char *digest = zck_get_index_digest(zck);
    zck_log(ZCK_LOG_DEBUG, "Index hash: (%s)", zck_hash_name_from_type(zck_get_full_hash_type(zck)));
    for(int i=0; i<zck_get_full_digest_size(zck); i++)
        zck_log(ZCK_LOG_DEBUG, "%02x", (unsigned char)digest[i]);
    zck_log(ZCK_LOG_DEBUG, "\n");
    if(!zck_read_comp_type(zck, dl->dst_fd))
        return False;
    start += 1;
    if(!zck_read_index_size(zck, dl->dst_fd))
        return False;
    start += sizeof(uint64_t);
    zck_log(ZCK_LOG_DEBUG, "Index size: %llu\n", zck->comp_index_size);
    if(!zck_dl_bytes(dl, url, zck->comp_index_size, start,
                     &buffer_len))
        return False;
    if(!zck_read_index(zck, dl->dst_fd))
        return False;
    zckIndexInfo *info = &(dl->info.index);
    info->hash_type = zck->index.hash_type;
    zck_log(ZCK_LOG_DEBUG, "Writing zeros to rest of file: %llu\n", zck->index.length + zck->comp_index_size + start);
    if(!zck_zero_bytes(dl, zck->index.length, zck->comp_index_size + start, &buffer_len))
        return False;
    return True;
}

size_t zck_dl_get_bytes_downloaded(zckDL *dl) {
    VALIDATE(dl);
    return dl->dl;
}

size_t zck_dl_get_bytes_uploaded(zckDL *dl) {
    VALIDATE(dl);
    return dl->ul;
}

void zck_dl_global_init() {
    curl_global_init(CURL_GLOBAL_ALL);
}

void zck_dl_global_cleanup() {
    curl_global_cleanup();
}