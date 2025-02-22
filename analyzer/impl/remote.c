/*

  Copyright (C) 2020 Gonzalo José Carracedo Carballal

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation, version 3.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this program.  If not, see
  <http://www.gnu.org/licenses/>

*/

#define SU_LOG_DOMAIN "remote-analyzer"

#include <sys/socket.h>

#include "remote.h"
#include "msg.h"

#include <fcntl.h>
#include <sys/poll.h>
#include <analyzer/realtime.h>
#include <netdb.h>

#ifdef bool
#  undef bool
#endif /* bool */

SUPRIVATE struct suscan_analyzer_interface *g_remote_analyzer_interface;

enum suscan_remote_analyzer_auth_result {
  SUSCAN_REMOTE_ANALYZER_AUTH_RESULT_SUCCESS,
  SUSCAN_REMOTE_ANALYZER_AUTH_RESULT_INVALID_SERVER,
  SUSCAN_REMOTE_ANALYZER_AUTH_RESULT_INCOMPATIBLE_VERSION,
  SUSCAN_REMOTE_ANALYZER_AUTH_RESULT_REJECTED,
  SUSCAN_REMOTE_ANALYZER_AUTH_RESULT_DISCONNECTED
};

#if 0
void
grow_buffer_debug(const grow_buf_t *buffer)
{
  unsigned int i;

  for (i = 0; i < buffer->size; ++i)
    fprintf(stderr, "%02x", buffer->bytes[i]);

  fprintf(stderr, "\n");
}
#endif

SUSCAN_SERIALIZER_PROTO(suscan_analyzer_server_hello) {
  SUSCAN_PACK_BOILERPLATE_START;

  SUSCAN_PACK(str,  self->server_name);
  SUSCAN_PACK(uint, self->protocol_version_major);
  SUSCAN_PACK(uint, self->protocol_version_minor);
  SUSCAN_PACK(uint, self->auth_mode);
  SUSCAN_PACK(uint, self->enc_type);
  SUSCAN_PACK(blob, self->sha256buf, SHA256_BLOCK_SIZE);

  SUSCAN_PACK_BOILERPLATE_END;
};

SUSCAN_DESERIALIZER_PROTO(suscan_analyzer_server_hello) {
  SUSCAN_UNPACK_BOILERPLATE_START;
  size_t size = 0;

  SUSCAN_UNPACK(str,   self->server_name);
  SUSCAN_UNPACK(uint8, self->protocol_version_major);
  SUSCAN_UNPACK(uint8, self->protocol_version_minor);
  SUSCAN_UNPACK(uint8, self->auth_mode);
  SUSCAN_UNPACK(uint8, self->enc_type);
  SUSCAN_UNPACK(blob,  self->sha256buf, &size);

  if (size != SHA256_BLOCK_SIZE) {
    SU_ERROR("Invalid salt size %d (expected %d)\n", size, SHA256_BLOCK_SIZE);
    goto fail;
  }

  SUSCAN_UNPACK_BOILERPLATE_END;
};

SUBOOL
suscan_analyzer_server_hello_init(
    struct suscan_analyzer_server_hello *self,
    const char *name)
{
  SUBOOL ok = SU_FALSE;
  unsigned int i;

  memset(self, 0, sizeof (struct suscan_analyzer_server_hello));

  SU_TRYCATCH(self->server_name = strdup(name), goto done);
  SU_TRYCATCH(self->sha256salt  = malloc(SHA256_BLOCK_SIZE), goto done);

  self->protocol_version_major = SUSCAN_REMOTE_PROTOCOL_MAJOR_VERSION;
  self->protocol_version_minor = SUSCAN_REMOTE_PROTOCOL_MINOR_VERSION;

  self->auth_mode = SUSCAN_REMOTE_AUTH_MODE_USER_PASSWORD;
  self->enc_type  = SUSCAN_REMOTE_ENC_TYPE_NONE;

  srand(suscan_gettime_raw());

  /* XXX: Find truly random bytes */
  for (i = 0; i < SHA256_BLOCK_SIZE; ++i)
    self->sha256salt[i] = rand();

  ok = SU_TRUE;

done:
  if (!ok)
    suscan_analyzer_server_hello_finalize(self);

  return ok;
}

void
suscan_analyzer_server_hello_finalize(
    struct suscan_analyzer_server_hello *self)
{
  if (self->sha256salt != NULL)
    free(self->sha256salt);

  if (self->server_name)
    free(self->server_name);
}

SUSCAN_SERIALIZER_PROTO(suscan_analyzer_server_client_auth) {
  SUSCAN_PACK_BOILERPLATE_START;

  SUSCAN_PACK(str,  self->client_name);
  SUSCAN_PACK(uint, self->protocol_version_major);
  SUSCAN_PACK(uint, self->protocol_version_minor);
  SUSCAN_PACK(str,  self->user);
  SUSCAN_PACK(blob, self->sha256buf, SHA256_BLOCK_SIZE);

  SUSCAN_PACK_BOILERPLATE_END;
}

SUSCAN_DESERIALIZER_PROTO(suscan_analyzer_server_client_auth) {
  SUSCAN_UNPACK_BOILERPLATE_START;
  size_t size = 0;

  SUSCAN_UNPACK(str,   self->client_name);
  SUSCAN_UNPACK(uint8, self->protocol_version_major);
  SUSCAN_UNPACK(uint8, self->protocol_version_minor);
  SUSCAN_UNPACK(str,   self->user);
  SUSCAN_UNPACK(blob,  self->sha256buf, &size);

  if (size != SHA256_BLOCK_SIZE) {
    SU_ERROR("Invalid token size %d (expected %d)\n", size, SHA256_BLOCK_SIZE);
    goto fail;
  }

  SUSCAN_UNPACK_BOILERPLATE_END;
}

void
suscan_analyzer_server_compute_auth_token(
    uint8_t *result,
    const char *user,
    const char *password,
    const uint8_t *sha256salt)
{
  SHA256_CTX ctx;

  suscan_sha256_init(&ctx);

  suscan_sha256_update(&ctx, (const BYTE *) user, strlen(user) + 1);
  suscan_sha256_update(&ctx, (const BYTE *) password, strlen(password) + 1);
  suscan_sha256_update(&ctx, (const BYTE *) sha256salt, SHA256_BLOCK_SIZE);

  suscan_sha256_final(&ctx, result);
}

SUBOOL
suscan_analyzer_server_client_auth_init(
    struct suscan_analyzer_server_client_auth *self,
    const struct suscan_analyzer_server_hello *hello,
    const char *name,
    const char *user,
    const char *password)
{
  SUBOOL ok = SU_FALSE;

  memset(self, 0, sizeof (struct suscan_analyzer_server_hello));

  SU_TRYCATCH(self->client_name  = strdup(name), goto done);
  SU_TRYCATCH(self->user         = strdup(user), goto done);
  SU_TRYCATCH(self->sha256token  = malloc(SHA256_BLOCK_SIZE), goto done);

  self->protocol_version_major = SUSCAN_REMOTE_PROTOCOL_MAJOR_VERSION;
  self->protocol_version_minor = SUSCAN_REMOTE_PROTOCOL_MINOR_VERSION;

  suscan_analyzer_server_compute_auth_token(
      self->sha256token,
      user,
      password,
      hello->sha256salt);

  ok = SU_TRUE;

done:
  if (!ok)
    suscan_analyzer_server_client_auth_finalize(self);

  return ok;
}

void
suscan_analyzer_server_client_auth_finalize(
    struct suscan_analyzer_server_client_auth *self)
{
  if (self->client_name != NULL)
    free(self->client_name);

  if (self->user != NULL)
    free(self->user);

  if (self->sha256token != NULL)
    free(self->sha256token);
}

SUSCAN_SERIALIZER_PROTO(suscan_analyzer_remote_call)
{
  SUSCAN_PACK_BOILERPLATE_START;

  SUSCAN_PACK(uint, self->type);

  switch (self->type) {
    case SUSCAN_ANALYZER_REMOTE_AUTH_INFO:
      SU_TRYCATCH(
          suscan_analyzer_server_client_auth_serialize(
              &self->client_auth,
              buffer),
          goto fail);
      break;

    case SUSCAN_ANALYZER_REMOTE_SOURCE_INFO:
      SU_TRYCATCH(
          suscan_analyzer_source_info_serialize(&self->source_info, buffer),
          goto fail);
      break;

    case SUSCAN_ANALYZER_REMOTE_SET_FREQUENCY:
      SUSCAN_PACK(freq, self->freq);
      SUSCAN_PACK(freq, self->lnb);
      break;

    case SUSCAN_ANALYZER_REMOTE_SET_GAIN:
      SUSCAN_PACK(str, self->gain.name);
      SUSCAN_PACK(float, self->gain.value);
      break;

    case SUSCAN_ANALYZER_REMOTE_SET_ANTENNA:
      SUSCAN_PACK(str, self->antenna);
      break;

    case SUSCAN_ANALYZER_REMOTE_SET_BANDWIDTH:
      SUSCAN_PACK(float, self->bandwidth);
      break;

    case SUSCAN_ANALYZER_REMOTE_SET_PPM:
      SUSCAN_PACK(float, self->ppm);
      break;

    case SUSCAN_ANALYZER_REMOTE_SET_DC_REMOVE:
      SUSCAN_PACK(bool, self->dc_remove);
      break;

    case SUSCAN_ANALYZER_REMOTE_SET_IQ_REVERSE:
      SUSCAN_PACK(bool, self->iq_reverse);
      break;

    case SUSCAN_ANALYZER_REMOTE_SET_AGC:
      SUSCAN_PACK(bool, self->agc);
      break;

    case SUSCAN_ANALYZER_REMOTE_FORCE_EOS:
      break;

    case SUSCAN_ANALYZER_REMOTE_SET_SWEEP_STRATEGY:
      SUSCAN_PACK(uint, self->sweep_strategy);
      break;

    case SUSCAN_ANALYZER_REMOTE_SET_SPECTRUM_PARTITIONING:
      SUSCAN_PACK(uint, self->spectrum_partitioning);
      break;

    case SUSCAN_ANALYZER_REMOTE_SET_HOP_RANGE:
      SUSCAN_PACK(freq, self->hop_range.min);
      SUSCAN_PACK(freq, self->hop_range.max);
      break;

    case SUSCAN_ANALYZER_REMOTE_SET_BUFFERING_SIZE:
      SUSCAN_PACK(uint, self->buffering_size);
      break;

    case SUSCAN_ANALYZER_REMOTE_MESSAGE:
      SU_TRYCATCH(
          suscan_analyzer_msg_serialize(self->msg.type, self->msg.ptr, buffer),
          goto fail);
      break;

    case SUSCAN_ANALYZER_REMOTE_REQ_HALT:
      break;

    case SUSCAN_ANALYZER_REMOTE_AUTH_REJECTED:
      break;

    default:
      SU_ERROR("Invalid remote call `%d'\n", self->type);
      break;
  }

  SUSCAN_PACK_BOILERPLATE_END;
}

SUSCAN_DESERIALIZER_PROTO(suscan_analyzer_remote_call)
{
  SUSCAN_UNPACK_BOILERPLATE_START;

  SUSCAN_UNPACK(uint32, self->type);

  switch (self->type) {
    case SUSCAN_ANALYZER_REMOTE_AUTH_INFO:
      SU_TRYCATCH(
          suscan_analyzer_server_client_auth_deserialize(
              &self->client_auth,
              buffer),
          goto fail);
      break;

    case SUSCAN_ANALYZER_REMOTE_SOURCE_INFO:
      SU_TRYCATCH(
          suscan_analyzer_source_info_deserialize(&self->source_info, buffer),
          goto fail);
      break;

    case SUSCAN_ANALYZER_REMOTE_SET_FREQUENCY:
      SUSCAN_UNPACK(freq, self->freq);
      SUSCAN_UNPACK(freq, self->lnb);
      break;

    case SUSCAN_ANALYZER_REMOTE_SET_GAIN:
      SUSCAN_UNPACK(str,   self->gain.name);
      SUSCAN_UNPACK(float, self->gain.value);
      break;

    case SUSCAN_ANALYZER_REMOTE_SET_ANTENNA:
      SUSCAN_UNPACK(str, self->antenna);
      break;

    case SUSCAN_ANALYZER_REMOTE_SET_BANDWIDTH:
      SUSCAN_UNPACK(float, self->bandwidth);
      break;

    case SUSCAN_ANALYZER_REMOTE_SET_PPM:
      SUSCAN_UNPACK(float, self->ppm);
      break;

    case SUSCAN_ANALYZER_REMOTE_SET_DC_REMOVE:
      SUSCAN_UNPACK(bool, self->dc_remove);
      break;

    case SUSCAN_ANALYZER_REMOTE_SET_IQ_REVERSE:
      SUSCAN_UNPACK(bool, self->iq_reverse);
      break;

    case SUSCAN_ANALYZER_REMOTE_SET_AGC:
      SUSCAN_UNPACK(bool, self->agc);
      break;

    case SUSCAN_ANALYZER_REMOTE_FORCE_EOS:
      break;

    case SUSCAN_ANALYZER_REMOTE_SET_SWEEP_STRATEGY:
      SUSCAN_UNPACK(uint32, self->sweep_strategy);
      SU_TRYCATCH(self->sweep_strategy < 2, goto fail);
      break;

    case SUSCAN_ANALYZER_REMOTE_SET_SPECTRUM_PARTITIONING:
      SUSCAN_UNPACK(uint32, self->spectrum_partitioning);
      SU_TRYCATCH(self->spectrum_partitioning < 2, goto fail);
      break;

    case SUSCAN_ANALYZER_REMOTE_SET_HOP_RANGE:
      SUSCAN_UNPACK(freq, self->hop_range.min);
      SUSCAN_UNPACK(freq, self->hop_range.max);

      SU_TRYCATCH(self->hop_range.min < self->hop_range.max, goto fail);
      break;

    case SUSCAN_ANALYZER_REMOTE_SET_BUFFERING_SIZE:
      SUSCAN_UNPACK(uint32, self->buffering_size);
      break;

    case SUSCAN_ANALYZER_REMOTE_MESSAGE:
      SU_TRYCATCH(
          suscan_analyzer_msg_deserialize(
              &self->msg.type,
              &self->msg.ptr,
              buffer),
          goto fail);

      break;

    case SUSCAN_ANALYZER_REMOTE_REQ_HALT:
      break;

    case SUSCAN_ANALYZER_REMOTE_AUTH_REJECTED:
      break;

    default:
      SU_ERROR("Invalid remote call `%d'\n", self->type);
      break;
  }

  SUSCAN_UNPACK_BOILERPLATE_END;
}

void
suscan_analyzer_remote_call_init(
    struct suscan_analyzer_remote_call *self,
    enum suscan_analyzer_remote_type type)
{
  memset(self, 0, sizeof(struct suscan_analyzer_remote_call));

  self->type = type;
}

SUBOOL
suscan_analyzer_remote_call_take_source_info(
    struct suscan_analyzer_remote_call *self,
    struct suscan_analyzer_source_info *info)
{
  SU_TRYCATCH(
      self->type == SUSCAN_ANALYZER_REMOTE_SOURCE_INFO,
      return SU_FALSE);

  suscan_analyzer_source_info_finalize(info);
  *info = self->source_info;
  self->type = SUSCAN_ANALYZER_REMOTE_NONE;

  return SU_TRUE;
}

SUBOOL
suscan_analyzer_remote_call_deliver_message(
    struct suscan_analyzer_remote_call *self,
    suscan_analyzer_t *analyzer)
{
  uint32_t type = 0;
  void *priv = NULL;

  SUBOOL ok = SU_FALSE;

  SU_TRYCATCH(
      self->type == SUSCAN_ANALYZER_REMOTE_MESSAGE,
      return SU_FALSE);

  type = self->msg.type;
  priv = self->msg.ptr;

  if (type == SUSCAN_ANALYZER_MESSAGE_TYPE_SOURCE_INFO) {
    suscan_analyzer_source_info_finalize(&self->source_info);
    SU_TRYCATCH(
        suscan_analyzer_source_info_init_copy(&self->source_info, priv),
        goto done);
  }

  SU_TRYCATCH(suscan_mq_write(analyzer->mq_out, type, priv), goto done);

  self->type = SUSCAN_ANALYZER_REMOTE_NONE;

  ok = SU_TRUE;

done:
  if (!ok && priv != NULL)
    suscan_analyzer_dispose_message(type, priv);

  return ok;
}


void
suscan_analyzer_remote_call_finalize(struct suscan_analyzer_remote_call *self)
{
  switch (self->type) {
    case SUSCAN_ANALYZER_REMOTE_AUTH_INFO:
      suscan_analyzer_server_client_auth_finalize(&self->client_auth);
      break;

    case SUSCAN_ANALYZER_REMOTE_SET_GAIN:
      if (self->gain.name != NULL)
        free(self->gain.name);
      break;

    case SUSCAN_ANALYZER_REMOTE_SET_ANTENNA:
      if (self->antenna != NULL)
        free(self->antenna);
      break;

    case SUSCAN_ANALYZER_REMOTE_SOURCE_INFO:
      suscan_analyzer_source_info_finalize(&self->source_info);
      break;

    case SUSCAN_ANALYZER_REMOTE_MESSAGE:
      /* Messages can become null if they are forwarded to the user */
      if (self->msg.ptr != NULL)
        suscan_analyzer_dispose_message(self->msg.type, self->msg.ptr);
      break;
  }

  self->type = SUSCAN_ANALYZER_REMOTE_NONE;
}

/**************************** Network thread **********************************/
size_t
suscan_remote_read(
    int sfd,
    int cancelfd,
    void *buffer,
    size_t size,
    int timeout_ms)
{
  uint8_t *as_bytes;
  char cancel_byte;
  size_t got = 0;
  size_t ret = 0;
  struct pollfd fds[2];

  fds[0].events  = POLLIN;
  fds[0].fd      = sfd;
  fds[0].revents = 0;

  fds[1].events  = POLLIN;
  fds[1].fd      = cancelfd;
  fds[1].revents = 0;

  as_bytes = (uint8_t *) buffer;

  while (size > 0) {
    SU_TRYCATCH(poll(fds, 2, timeout_ms) != -1, return -1);

    if (fds[1].revents & POLLIN) {
      (void) read(cancelfd, &cancel_byte, 1);
      errno = ECANCELED;
      return -1;
    } else if (fds[0].revents & POLLIN) {
      ret = read(sfd, as_bytes, size);

      if (ret == 0)
        goto done;
      else if (ret == -1)
        return -1; /* Connection error, return immediately */

      got      += ret;
      as_bytes += ret;
      size     -= ret;
    } else {
      /* Connection error due to timeout, return immediately */
      errno = ETIMEDOUT;
      return -1;
    }
  }

done:
  return got;
}

SUBOOL
suscan_remote_read_pdu(
    int sfd,
    int cancelfd,
    grow_buf_t *buffer,
    int timeout_ms)
{
  uint32_t chunksiz;
  struct suscan_analyzer_remote_pdu_header header;
  void *chunk;
  size_t got;
  SUBOOL ok = SU_FALSE;

  grow_buf_clear(buffer);

  /* Attempt to read header */
  SU_TRYCATCH(
      (got = suscan_remote_read(
          sfd,
          cancelfd,
          &header,
          sizeof(struct suscan_analyzer_remote_pdu_header),
          timeout_ms)) == sizeof(struct suscan_analyzer_remote_pdu_header),
      goto done);

  header.size  = ntohl(header.size);
  header.magic = ntohl(header.magic);

  if (header.magic != SUSCAN_REMOTE_PDU_HEADER_MAGIC) {
    SU_ERROR("Protocol error (unrecognized PDU magic)\n");
    goto done;
  }

  /* Start to read */
  while (header.size > 0) {
    chunksiz = header.size;
    if (chunksiz > SUSCAN_REMOTE_READ_BUFFER)
      chunksiz = SUSCAN_REMOTE_READ_BUFFER;

    SU_TRYCATCH(chunk = grow_buf_alloc(buffer, chunksiz), goto done);
    SU_TRYCATCH(
        suscan_remote_read(
            sfd,
            cancelfd,
            chunk,
            chunksiz,
            SUSCAN_REMOTE_ANALYZER_PDU_BODY_TIMEOUT_MS) == chunksiz,
        goto done);

    /*
     * No need to advance growbuf pointer. We are just incrementing
     * its size.
     */
    header.size -= chunksiz;
  }

  ok = SU_TRUE;

done:
  return ok;
}

SUBOOL
suscan_remote_write_pdu(
    int sfd,
    const grow_buf_t *buffer)
{
  uint8_t *buffer_bytes = grow_buf_get_buffer(buffer);
  size_t   buffer_size  = grow_buf_get_size(buffer);
  size_t   chunksize;

  struct suscan_analyzer_remote_pdu_header header;

  header.magic = htonl(SUSCAN_REMOTE_PDU_HEADER_MAGIC);
  header.size  = htonl(buffer_size);

  if (write(sfd, &header, sizeof(struct suscan_analyzer_remote_pdu_header))
      != sizeof(struct suscan_analyzer_remote_pdu_header)) {
    SU_ERROR("Protocol header write error\n");
    return SU_FALSE;
  }

  while (buffer_size > 0) {
    chunksize = buffer_size;

    if (chunksize > SUSCAN_REMOTE_READ_BUFFER)
      chunksize = SUSCAN_REMOTE_READ_BUFFER;

    if (write(sfd, buffer_bytes, chunksize) != chunksize) {
      SU_ERROR("Protocol body write error\n");
      return SU_FALSE;
    }

    buffer_size  -= chunksize;
    buffer_bytes += chunksize;
  }

  return SU_TRUE;
}

/*
 * User is in charge of releasing the pointer
 */
SUPRIVATE struct suscan_analyzer_remote_call *
suscan_remote_analyzer_receive_call(
    suscan_remote_analyzer_t *self,
    int sfd,
    int cancelfd,
    int timeout_ms)
{
  struct suscan_analyzer_remote_call *call = NULL;
  SUBOOL ok = SU_FALSE;

  /* Right now: use control fd only */
  SU_TRYCATCH(
      suscan_remote_read_pdu(
          self->peer.control_fd,
          self->cancel_pipe[0],
          &self->peer.read_buffer,
          timeout_ms),
      goto done);

  call = suscan_remote_analyzer_acquire_call(
      self,
      SUSCAN_ANALYZER_REMOTE_NONE);

  SU_TRYCATCH(
      suscan_analyzer_remote_call_deserialize(call, &self->peer.read_buffer),
      goto done);

  ok = SU_TRUE;

done:
  if (!ok && call != NULL) {
      (void) suscan_remote_analyzer_release_call(self, call);
      call = NULL;
  }

  return call;
}

SUPRIVATE SUBOOL
suscan_remote_analyzer_deliver_call(
    suscan_remote_analyzer_t *self,
    int sfd,
    struct suscan_analyzer_remote_call *call)
{
  SUBOOL serialized = SU_FALSE;
  SUBOOL ok = SU_FALSE;

  SU_TRYCATCH(
      suscan_analyzer_remote_call_serialize(call, &self->peer.write_buffer),
      goto done);
  serialized = SU_TRUE;

  SU_TRYCATCH(suscan_remote_analyzer_release_call(self, call), goto done);

  SU_TRYCATCH(
      suscan_remote_write_pdu(self->peer.control_fd, &self->peer.write_buffer),
      goto done);

  ok = SU_TRUE;

done:
  if (!serialized)
    (void) suscan_remote_analyzer_release_call(self, call);

  return ok;
}

SUPRIVATE int
suscan_remote_analyzer_network_connect_cancellable(
    struct in_addr ipaddr,
    uint16_t port,
    int cancelfd,
    int timeout_ms)
{
  struct sockaddr_in addr;
  struct pollfd fds[2];

  int sfd = -1;
  int ret = -1;
  int sockerr;
  socklen_t socklen;
  int flags;
  char cancel_byte;

  SU_TRYCATCH((sfd = socket(AF_INET, SOCK_STREAM, 0)) != -1, goto done);

  addr.sin_family = AF_INET;
  addr.sin_addr   = ipaddr;
  addr.sin_port   = htons(port);

  SU_TRYCATCH((flags = fcntl(sfd, F_GETFL, NULL)) != -1, goto done);
  flags |= O_NONBLOCK;
  SU_TRYCATCH(fcntl(sfd, F_SETFL, flags) != -1, goto done);

  ret = connect(sfd, (struct sockaddr *) &addr, sizeof(struct sockaddr_in));

  if (ret == -1) {
    SU_TRYCATCH(errno == EINPROGRESS, goto done);

    /* Inspect the socket. Connection is ready as soon as we can write on it */
    fds[0].events = POLLOUT;
    fds[0].fd     = sfd;

    /* Also inspect the cancellation fd. It being readable means cancellation */
    fds[1].events = POLLIN;
    fds[1].fd     = cancelfd;

    ret = poll(fds, 2, timeout_ms);

    switch (ret) {
      case 0:
        /* None ready, timed out */
        ret = -1;
        errno = ETIMEDOUT;
        goto done;

      case -1:
        /* Poll failed (socket weirdness?) */
        goto done;

      default:
        if (fds[1].revents & POLLIN) {
          /* Cancel requested */
          (void) read(cancelfd, &cancel_byte, 1);
          ret = -1;
          errno = ECANCELED;
          goto done;
        } else if (fds[0].revents & POLLOUT) {
          sockerr = 0;
          socklen = sizeof(int);

          SU_TRYCATCH(
              getsockopt(sfd, SOL_SOCKET, SO_ERROR, &sockerr, &socklen) != -1,
              goto done);

          if (sockerr != 0) {
            ret = -1;
            errno = sockerr;
            goto done;
          }
        } else {
          SU_ERROR("Invalid socket condition\n");
          ret = -1;
          goto done;
        }
    }
  }

  ret = sfd;
  sfd = -1;

  /*
   * Socket is left in nonblock mode. This is mandatory in order to perform
   * polling between multiple descriptors, including the cancellation
   * descriptor.
   */
done:
  if (sfd != -1)
    close(sfd);

  return ret;
}

SUPRIVATE enum suscan_remote_analyzer_auth_result
suscan_remote_analyzer_auth_peer(suscan_remote_analyzer_t *self)
{
  struct suscan_analyzer_remote_call *call = NULL;
  struct suscan_analyzer_server_hello hello;
  char hostname[64];
  SUBOOL write_ok = SU_FALSE;
  enum suscan_remote_analyzer_auth_result result =
      SUSCAN_REMOTE_ANALYZER_AUTH_RESULT_INVALID_SERVER;

  memset(&hello, 0, sizeof(struct suscan_analyzer_server_hello));

  SU_TRYCATCH(
      suscan_remote_read_pdu(
          self->peer.control_fd,
          self->cancel_pipe[0],
          &self->peer.read_buffer,
          SUSCAN_REMOTE_ANALYZER_AUTH_TIMEOUT_MS),
      goto done);

  SU_TRYCATCH(
      suscan_analyzer_server_hello_deserialize(&hello, &self->peer.read_buffer),
      goto done);

  if (hello.protocol_version_major < SUSCAN_REMOTE_PROTOCOL_MAJOR_VERSION) {
    result = SUSCAN_REMOTE_ANALYZER_AUTH_RESULT_INCOMPATIBLE_VERSION;
    SU_ERROR(
        "Remote server is too old (protocol version %d.%d)\n",
        hello.protocol_version_major,
        hello.protocol_version_minor);
    goto done;
  }

  SU_TRYCATCH(
      call = suscan_remote_analyzer_acquire_call(
          self,
          SUSCAN_ANALYZER_REMOTE_AUTH_INFO),
      goto done);

  /* Prepare authentication message */
  (void) gethostname(hostname, sizeof(hostname));
  hostname[sizeof(hostname) - 1] = '\0';

  SU_TRYCATCH(
      suscan_analyzer_server_client_auth_init(
          &call->client_auth,
          &hello,
          hostname,
          self->peer.user,
          self->peer.password),
      goto done);

  write_ok = suscan_remote_analyzer_deliver_call(
      self,
      self->peer.control_fd,
      call);
  call = NULL;

  result = SUSCAN_REMOTE_ANALYZER_AUTH_RESULT_DISCONNECTED;

  SU_TRYCATCH(write_ok, goto done);

  SU_TRYCATCH(
      call = suscan_remote_analyzer_receive_call(
          self,
          self->peer.control_fd,
          self->cancel_pipe[0],
          SUSCAN_REMOTE_ANALYZER_AUTH_TIMEOUT_MS),
      goto done);

  if (call->type == SUSCAN_ANALYZER_REMOTE_AUTH_REJECTED) {
    result = SUSCAN_REMOTE_ANALYZER_AUTH_RESULT_REJECTED;
    goto done;
  }

  SU_TRYCATCH(call->type == SUSCAN_ANALYZER_REMOTE_SOURCE_INFO, goto done);
  SU_TRYCATCH(
      suscan_analyzer_remote_call_take_source_info(
          call,
          &self->source_info),
      goto done);
  SU_TRYCATCH(
      suscan_analyzer_send_source_info(self->parent, &self->source_info),
      goto done);

  /* TODO: Warn client about new source info */
  suscan_remote_analyzer_release_call(self, call);
  call = NULL;

  result = SUSCAN_REMOTE_ANALYZER_AUTH_RESULT_SUCCESS;

done:
  if (call != NULL)
    suscan_remote_analyzer_release_call(self, call);

  suscan_analyzer_server_hello_finalize(&hello);

  return result;
}

SUPRIVATE SUBOOL
suscan_remote_analyzer_connect_to_peer(suscan_remote_analyzer_t *self)
{
  struct hostent *ent;
  enum suscan_remote_analyzer_auth_result auth_result;
  SUBOOL ok = SU_FALSE;

  SU_TRYCATCH(
      suscan_analyzer_send_status(
          self->parent,
          SUSCAN_ANALYZER_MESSAGE_TYPE_SOURCE_INIT,
          SUSCAN_ANALYZER_INIT_PROGRESS,
          "Resolving remote host `%s'...",
          self->peer.hostname),
      goto done);

  if ((ent = gethostbyname(self->peer.hostname)) == NULL
      || ent->h_length == 0) {
    (void) suscan_analyzer_send_status(
        self->parent,
        SUSCAN_ANALYZER_MESSAGE_TYPE_SOURCE_INIT,
        SUSCAN_ANALYZER_INIT_FAILURE,
        "Cannot resolve host `%s'",
        self->peer.hostname);
    goto done;
  }

  self->peer.hostaddr = *((struct in_addr *) ent->h_addr_list[0]);

  SU_TRYCATCH(
      suscan_analyzer_send_status(
          self->parent,
          SUSCAN_ANALYZER_MESSAGE_TYPE_SOURCE_INIT,
          SUSCAN_ANALYZER_INIT_PROGRESS,
          "Host name resolved, connecting to control server on port %d...",
          self->peer.port),
      goto done);

  self->peer.control_fd = suscan_remote_analyzer_network_connect_cancellable(
      self->peer.hostaddr,
      self->peer.port,
      self->cancel_pipe[0],
      SUSCAN_REMOTE_ANALYZER_CONNECT_TIMEOUT_MS);

  if (self->peer.control_fd == -1) {
    (void) suscan_analyzer_send_status(
        self->parent,
        SUSCAN_ANALYZER_MESSAGE_TYPE_SOURCE_INIT,
        SUSCAN_ANALYZER_INIT_FAILURE,
        "Cannot connect to %s:%d (TCP): %s",
        self->peer.hostname,
        self->peer.port,
        strerror(errno));
    goto done;
  }

  SU_TRYCATCH(
    suscan_analyzer_send_status(
        self->parent,
        SUSCAN_ANALYZER_MESSAGE_TYPE_SOURCE_INIT,
        SUSCAN_ANALYZER_INIT_PROGRESS,
        "Connection successful. Authenticating against peer...",
        self->peer.port),
    goto done);

  auth_result = suscan_remote_analyzer_auth_peer(self);

  switch (auth_result) {
    case SUSCAN_REMOTE_ANALYZER_AUTH_RESULT_DISCONNECTED:
      (void) suscan_analyzer_send_status(
          self->parent,
          SUSCAN_ANALYZER_MESSAGE_TYPE_SOURCE_INIT,
          SUSCAN_ANALYZER_INIT_FAILURE,
          "Connection reset during authentication");
      goto done;

    case SUSCAN_REMOTE_ANALYZER_AUTH_RESULT_INCOMPATIBLE_VERSION:
      (void) suscan_analyzer_send_status(
          self->parent,
          SUSCAN_ANALYZER_MESSAGE_TYPE_SOURCE_INIT,
          SUSCAN_ANALYZER_INIT_FAILURE,
          "Incompatible server protocol");
      goto done;

    case SUSCAN_REMOTE_ANALYZER_AUTH_RESULT_INVALID_SERVER:
      (void) suscan_analyzer_send_status(
          self->parent,
          SUSCAN_ANALYZER_MESSAGE_TYPE_SOURCE_INIT,
          SUSCAN_ANALYZER_INIT_FAILURE,
          "Connection opened, but host is not a valid Suscan device server");
      goto done;

    case SUSCAN_REMOTE_ANALYZER_AUTH_RESULT_REJECTED:
      (void) suscan_analyzer_send_status(
          self->parent,
          SUSCAN_ANALYZER_MESSAGE_TYPE_SOURCE_INIT,
          SUSCAN_ANALYZER_INIT_FAILURE,
          "Authentication rejected (wrong user and/or password?)");
      goto done;

    case SUSCAN_REMOTE_ANALYZER_AUTH_RESULT_SUCCESS:
      break;
  }

  SU_TRYCATCH(
    suscan_analyzer_send_status(
        self->parent,
        SUSCAN_ANALYZER_MESSAGE_TYPE_SOURCE_INIT,
        SUSCAN_ANALYZER_INIT_SUCCESS,
        NULL),
    goto done);


  ok = SU_TRUE;

done:
  return ok;
}


SUPRIVATE void *
suscan_remote_analyzer_rx_thread(void *ptr)
{
  suscan_remote_analyzer_t *self = (suscan_remote_analyzer_t *) ptr;

  struct suscan_analyzer_remote_call *call;

  while ((call = suscan_remote_analyzer_receive_call(
      self,
      self->peer.control_fd,
      self->cancel_pipe[0],
      -1)) != NULL) {
    switch (call->type) {
      case SUSCAN_ANALYZER_REMOTE_SOURCE_INFO:
        SU_TRYCATCH(
            suscan_analyzer_remote_call_take_source_info(
                call,
                &self->source_info),
            goto done);

        SU_TRYCATCH(
            suscan_analyzer_send_source_info(self->parent, &self->source_info),
            goto done);
        break;

      case SUSCAN_ANALYZER_REMOTE_FORCE_EOS:
        self->parent->eos = SU_TRUE;
        suscan_analyzer_send_status(
            self->parent,
            SUSCAN_ANALYZER_MESSAGE_TYPE_EOS,
            0,
            "End of stream reached");
        goto done;

      case SUSCAN_ANALYZER_REMOTE_MESSAGE:
        SU_TRYCATCH(
            suscan_analyzer_remote_call_deliver_message(call, self->parent),
            goto done);
        break;
    }

    suscan_remote_analyzer_release_call(self, call);
    call = NULL;
  }

done:
  if (call != NULL)
    suscan_remote_analyzer_release_call(self, call);

  (void) suscan_mq_write(&self->pdu_queue, SUSCAN_REMOTE_HALT, NULL);

  return NULL;
}

SUPRIVATE void *
suscan_remote_analyzer_tx_thread(void *ptr)
{
  suscan_remote_analyzer_t *self = (suscan_remote_analyzer_t *) ptr;
  uint32_t is_ctl = 0;
  grow_buf_t *as_growbuf = NULL;
  void *msgptr = NULL;

  SU_TRYCATCH(suscan_remote_analyzer_connect_to_peer(self), goto done);

  SU_TRYCATCH(
      pthread_create(
          &self->rx_thread,
          NULL,
          suscan_remote_analyzer_rx_thread,
          self) != -1,
      goto done);
  self->rx_thread_init = SU_TRUE;

  while ((msgptr = suscan_mq_read(&self->pdu_queue, &is_ctl)) != NULL) {
    switch (is_ctl) {
      case SU_TRUE:
      case SU_FALSE:
        as_growbuf = (grow_buf_t *) msgptr;

        /* We only support control messages for now */
        SU_TRYCATCH(
            suscan_remote_write_pdu(self->peer.control_fd, msgptr),
            goto done);

        grow_buf_finalize(as_growbuf);
        free(as_growbuf);
        as_growbuf = NULL;
        break;

      case SUSCAN_REMOTE_HALT:
        goto done;
    }
  }

done:
  self->parent->running = SU_FALSE;

  if (as_growbuf != NULL) {
    grow_buf_finalize(as_growbuf);
    free(as_growbuf);
  }

  suscan_mq_write_urgent(
      self->parent->mq_out,
      SUSCAN_WORKER_MSG_TYPE_HALT,
      NULL);

  return NULL;
}

/***************************** Call queueing **********************************/
struct suscan_analyzer_remote_call *
suscan_remote_analyzer_acquire_call(
    suscan_remote_analyzer_t *self,
    enum suscan_analyzer_remote_type type)
{
  SU_TRYCATCH(pthread_mutex_lock(&self->call_mutex) == 0, return NULL);

  suscan_analyzer_remote_call_init(&self->call, type);

  return &self->call;
}

SUBOOL
suscan_remote_analyzer_release_call(
    suscan_remote_analyzer_t *self,
    struct suscan_analyzer_remote_call *call)
{
  SU_TRYCATCH(call == &self->call, return SU_FALSE);

  suscan_analyzer_remote_call_finalize(&self->call);

  SU_TRYCATCH(pthread_mutex_unlock(&self->call_mutex) == 0, return SU_FALSE);

  return SU_TRUE;
}


SUBOOL
suscan_remote_analyzer_queue_call(
    suscan_remote_analyzer_t *self,
    struct suscan_analyzer_remote_call *call,
    SUBOOL is_control)
{
  grow_buf_t *buf = NULL;
  SUBOOL ok = SU_FALSE;

  SU_TRYCATCH(buf = calloc(1, sizeof(grow_buf_t)), goto done);
  SU_TRYCATCH(
      suscan_analyzer_remote_call_serialize(&self->call, buf),
      goto done);

  SU_TRYCATCH(suscan_mq_write(&self->pdu_queue, is_control, buf), goto done);

  ok = SU_TRUE;

done:
  if (!ok) {
    grow_buf_finalize(buf);
    free(buf);
  }
  return ok;
}

/*************************** Analyzer interface *******************************/
SUPRIVATE void suscan_remote_analyzer_dtor(void *ptr);

SUPRIVATE void
suscan_remote_analyzer_consume_pdu_queue(suscan_remote_analyzer_t *self)
{
  grow_buf_t *buffer;
  uint32_t type;

  while (suscan_mq_poll(&self->pdu_queue, &type, (void **) &buffer)) {
    if (type != SUSCAN_REMOTE_HALT) {
      grow_buf_finalize(buffer);
      free(buffer);
    }
  }
}

void *
suscan_remote_analyzer_ctor(suscan_analyzer_t *parent, va_list ap)
{
  suscan_remote_analyzer_t *new = NULL;
  suscan_source_config_t *config;
  const char *val;
  const char *portstr;
  unsigned int port;

  config = va_arg(ap, suscan_source_config_t *);

#if 0
  if ((driver = suscan_source_config_get_param(config, "transport")) == NULL) {
    SU_ERROR("Cannot initialize remote source: no driver specified\n");
    goto fail;
  }

  if (strcmp(driver, "transport") != 0) {
    SU_ERROR(
        "Cannot initialize remote source: unsupported driver `%s'\n",
        driver);
    goto fail;
  }
#endif

  SU_TRYCATCH(new = calloc(1, sizeof(suscan_remote_analyzer_t)), goto fail);

  /*
   * We tentatively set this to the configured sample rate. It may change
   * later but at least it makes the user believe this is a regular analyzer.
   */
  new->source_info.source_samp_rate =
      suscan_source_config_get_samp_rate(config);

  new->parent = parent;
  new->peer.control_fd = -1;
  new->peer.data_fd    = -1;
  new->cancel_pipe[0]  = -1;
  new->cancel_pipe[1]  = -1;

  val = suscan_source_config_get_param(config, "host");
  if (val == NULL) {
    SU_ERROR("Cannot initialize remote source: no remote host provided\n");
    goto fail;
  }
  SU_TRYCATCH(new->peer.hostname = strdup(val), goto fail);

  portstr = suscan_source_config_get_param(config, "port");
  if (portstr == NULL) {
    SU_ERROR("Cannot initialize remote source: no remote port provided\n");
    goto fail;
  }

  if (sscanf(portstr, "%u", &port) < 1 || port > 65535) {
    SU_ERROR("Cannot initialize remote source: invalid port\n");
    goto fail;
  }
  new->peer.port = port;

  val = suscan_source_config_get_param(config, "user");
  if (val == NULL) {
    SU_ERROR("No username provided\n");
    goto fail;
  }
  SU_TRYCATCH(new->peer.user = strdup(val), goto fail);

  val = suscan_source_config_get_param(config, "password");
  if (val == NULL) {
    SU_ERROR("No password provided\n");
    goto fail;
  }
  SU_TRYCATCH(new->peer.password = strdup(val), goto fail);

  SU_TRYCATCH(pthread_mutex_init(&new->call_mutex, NULL) == 0, goto fail);
  new->call_mutex_initialized = SU_TRUE;

  SU_TRYCATCH(pipe(new->cancel_pipe) != -1, goto fail);

  SU_TRYCATCH(
      pthread_create(
          &new->tx_thread,
          NULL,
          suscan_remote_analyzer_tx_thread,
          new) != -1,
      goto fail);
  new->tx_thread_init = SU_TRUE;

  return new;

fail:
  if (new != NULL)
    suscan_remote_analyzer_dtor(new);

  return NULL;
}

SUPRIVATE void
suscan_remote_analyzer_dtor(void *ptr)
{
  suscan_remote_analyzer_t *self = (suscan_remote_analyzer_t *) ptr;
  char b = 1;

  if (self->tx_thread_init) {
    if (self->rx_thread_init) {
      write(self->cancel_pipe[1], &b, 1);
      pthread_join(self->rx_thread, NULL);
    }

    suscan_mq_write(&self->pdu_queue, SUSCAN_REMOTE_HALT, NULL);
    pthread_join(self->tx_thread, NULL);
  }

  if (self->peer.hostname != NULL)
    free(self->peer.hostname);

  if (self->peer.user != NULL)
    free(self->peer.user);

  if (self->peer.password != NULL)
    free(self->peer.password);

  if (self->peer.control_fd != -1)
    close(self->peer.control_fd);

  if (self->peer.data_fd != -1)
    close(self->peer.data_fd);

  if (self->call_mutex_initialized)
    pthread_mutex_destroy(&self->call_mutex);

  suscan_remote_analyzer_consume_pdu_queue(self);

  if (self->cancel_pipe[0] != -1)
    close(self->cancel_pipe[0]);

  if (self->cancel_pipe[1] != -1)
    close(self->cancel_pipe[1]);

  free(self);
}

/* Source-related methods */
SUPRIVATE SUBOOL
suscan_remote_analyzer_set_frequency(void *ptr, SUFREQ freq, SUFREQ lnb)
{
  suscan_remote_analyzer_t *self = (suscan_remote_analyzer_t *) ptr;
  struct suscan_analyzer_remote_call *call = NULL;
  SUBOOL ok = SU_FALSE;

  SU_TRYCATCH(
      call = suscan_remote_analyzer_acquire_call(
          self,
          SUSCAN_ANALYZER_REMOTE_SET_FREQUENCY),
      goto done);

  call->freq = freq;
  call->lnb  = lnb;

  SU_TRYCATCH(
      suscan_remote_analyzer_queue_call(self, call, SU_TRUE),
      goto done);

  ok = SU_TRUE;

done:
  if (call != NULL)
    suscan_remote_analyzer_release_call(self, call);

  return ok;
}

SUPRIVATE SUBOOL
suscan_remote_analyzer_set_gain(void *ptr, const char *name, SUFLOAT value)
{
  suscan_remote_analyzer_t *self = (suscan_remote_analyzer_t *) ptr;
  struct suscan_analyzer_remote_call *call = NULL;
  SUBOOL ok = SU_FALSE;

  SU_TRYCATCH(
      call = suscan_remote_analyzer_acquire_call(
          self,
          SUSCAN_ANALYZER_REMOTE_SET_GAIN),
      goto done);

  SU_TRYCATCH(call->gain.name = strdup(name), goto done);
  call->gain.value = value;

  SU_TRYCATCH(
      suscan_remote_analyzer_queue_call(self, call, SU_TRUE),
      goto done);

  ok = SU_TRUE;

done:
  if (call != NULL)
    suscan_remote_analyzer_release_call(self, call);

  return ok;
}

SUPRIVATE SUBOOL
suscan_remote_analyzer_set_antenna(void *ptr, const char *name)
{
  suscan_remote_analyzer_t *self = (suscan_remote_analyzer_t *) ptr;
  struct suscan_analyzer_remote_call *call = NULL;
  SUBOOL ok = SU_FALSE;

  SU_TRYCATCH(
      call = suscan_remote_analyzer_acquire_call(
          self,
          SUSCAN_ANALYZER_REMOTE_SET_ANTENNA),
      goto done);

  SU_TRYCATCH(call->antenna = strdup(name), goto done);

  SU_TRYCATCH(
      suscan_remote_analyzer_queue_call(self, call, SU_TRUE),
      goto done);

  ok = SU_TRUE;

done:
  if (call != NULL)
    suscan_remote_analyzer_release_call(self, call);

  return ok;
}

SUPRIVATE SUBOOL
suscan_remote_analyzer_set_bandwidth(void *ptr, SUFLOAT value)
{
  suscan_remote_analyzer_t *self = (suscan_remote_analyzer_t *) ptr;
  struct suscan_analyzer_remote_call *call = NULL;
  SUBOOL ok = SU_FALSE;

  SU_TRYCATCH(
      call = suscan_remote_analyzer_acquire_call(
          self,
          SUSCAN_ANALYZER_REMOTE_SET_BANDWIDTH),
      goto done);

  call->bandwidth = value;

  SU_TRYCATCH(
      suscan_remote_analyzer_queue_call(self, call, SU_TRUE),
      goto done);

  ok = SU_TRUE;

done:
  if (call != NULL)
    suscan_remote_analyzer_release_call(self, call);

  return ok;
}

SUPRIVATE SUBOOL
suscan_remote_analyzer_set_ppm(void *ptr, SUFLOAT value)
{
  suscan_remote_analyzer_t *self = (suscan_remote_analyzer_t *) ptr;
  struct suscan_analyzer_remote_call *call = NULL;
  SUBOOL ok = SU_FALSE;

  SU_TRYCATCH(
      call = suscan_remote_analyzer_acquire_call(
          self,
          SUSCAN_ANALYZER_REMOTE_SET_PPM),
      goto done);

  call->ppm = value;

  SU_TRYCATCH(
      suscan_remote_analyzer_queue_call(self, call, SU_TRUE),
      goto done);

  ok = SU_TRUE;

done:
  if (call != NULL)
    suscan_remote_analyzer_release_call(self, call);

  return ok;
}

SUPRIVATE SUBOOL
suscan_remote_analyzer_set_dc_remove(void *ptr, SUBOOL value)
{
  suscan_remote_analyzer_t *self = (suscan_remote_analyzer_t *) ptr;
  struct suscan_analyzer_remote_call *call = NULL;
  SUBOOL ok = SU_FALSE;

  SU_TRYCATCH(
      call = suscan_remote_analyzer_acquire_call(
          self,
          SUSCAN_ANALYZER_REMOTE_SET_DC_REMOVE),
      goto done);

  call->dc_remove = value;

  SU_TRYCATCH(
      suscan_remote_analyzer_queue_call(self, call, SU_TRUE),
      goto done);

  ok = SU_TRUE;

done:
  if (call != NULL)
    suscan_remote_analyzer_release_call(self, call);

  return ok;
}

SUPRIVATE SUBOOL
suscan_remote_analyzer_set_iq_reverse(void *ptr, SUBOOL value)
{
  suscan_remote_analyzer_t *self = (suscan_remote_analyzer_t *) ptr;
  struct suscan_analyzer_remote_call *call = NULL;
  SUBOOL ok = SU_FALSE;

  SU_TRYCATCH(
      call = suscan_remote_analyzer_acquire_call(
          self,
          SUSCAN_ANALYZER_REMOTE_SET_IQ_REVERSE),
      goto done);

  call->iq_reverse = value;

  SU_TRYCATCH(
      suscan_remote_analyzer_queue_call(self, call, SU_TRUE),
      goto done);

  ok = SU_TRUE;

done:
  if (call != NULL)
    suscan_remote_analyzer_release_call(self, call);

  return ok;

  return SU_TRUE;
}

SUPRIVATE SUBOOL
suscan_remote_analyzer_set_agc(void *ptr, SUBOOL value)
{
  suscan_remote_analyzer_t *self = (suscan_remote_analyzer_t *) ptr;
  struct suscan_analyzer_remote_call *call = NULL;
  SUBOOL ok = SU_FALSE;

  SU_TRYCATCH(
      call = suscan_remote_analyzer_acquire_call(
          self,
          SUSCAN_ANALYZER_REMOTE_SET_AGC),
      goto done);

  call->agc = value;

  SU_TRYCATCH(
      suscan_remote_analyzer_queue_call(self, call, SU_TRUE),
      goto done);

  ok = SU_TRUE;

done:
  if (call != NULL)
    suscan_remote_analyzer_release_call(self, call);

  return ok;
}

SUPRIVATE SUBOOL
suscan_remote_analyzer_force_eos(void *ptr)
{
  suscan_remote_analyzer_t *self = (suscan_remote_analyzer_t *) ptr;
  struct suscan_analyzer_remote_call *call = NULL;
  SUBOOL ok = SU_FALSE;

  SU_TRYCATCH(
      call = suscan_remote_analyzer_acquire_call(
          self,
          SUSCAN_ANALYZER_REMOTE_FORCE_EOS),
      goto done);

  /* TODO: Implement me */

  SU_TRYCATCH(
      suscan_remote_analyzer_queue_call(self, call, SU_TRUE),
      goto done);

  ok = SU_TRUE;

done:
  if (call != NULL)
    suscan_remote_analyzer_release_call(self, call);

  return ok;
}

SUPRIVATE SUBOOL
suscan_remote_analyzer_is_real_time(const void *ptr)
{
  return SU_TRUE;
}

SUPRIVATE unsigned int
suscan_remote_analyzer_get_samp_rate(const void *ptr)
{
  suscan_remote_analyzer_t *self = (suscan_remote_analyzer_t *) ptr;

  return self->source_info.source_samp_rate;
}

SUPRIVATE SUFLOAT
suscan_remote_analyzer_get_measured_samp_rate(const void *ptr)
{
  const suscan_remote_analyzer_t *self = (const suscan_remote_analyzer_t *) ptr;

  return self->source_info.measured_samp_rate;
}

SUPRIVATE struct suscan_analyzer_source_info *
suscan_remote_analyzer_get_source_info_pointer(const void *ptr)
{
  suscan_remote_analyzer_t *self = (suscan_remote_analyzer_t *) ptr;

  return &self->source_info;
}

SUPRIVATE SUBOOL
suscan_remote_analyzer_commit_source_info(void *ptr)
{
  return SU_TRUE;
}

/* Worker specific methods */
SUPRIVATE SUBOOL
suscan_remote_analyzer_set_sweep_strategy(
    void *ptr,
    enum suscan_analyzer_sweep_strategy strategy)
{
  suscan_remote_analyzer_t *self = (suscan_remote_analyzer_t *) ptr;
  struct suscan_analyzer_remote_call *call = NULL;
  SUBOOL ok = SU_FALSE;

  SU_TRYCATCH(
      call = suscan_remote_analyzer_acquire_call(
          self,
          SUSCAN_ANALYZER_REMOTE_SET_SWEEP_STRATEGY),
      goto done);

  call->sweep_strategy = strategy;

  SU_TRYCATCH(
      suscan_remote_analyzer_queue_call(self, call, SU_TRUE),
      goto done);

  ok = SU_TRUE;

done:
  if (call != NULL)
    suscan_remote_analyzer_release_call(self, call);

  return ok;
}

SUPRIVATE SUBOOL
suscan_remote_analyzer_set_spectrum_partitioning(
    void *ptr,
    enum suscan_analyzer_spectrum_partitioning partitioning)
{
  suscan_remote_analyzer_t *self = (suscan_remote_analyzer_t *) ptr;
  struct suscan_analyzer_remote_call *call = NULL;
  SUBOOL ok = SU_FALSE;

  SU_TRYCATCH(
      call = suscan_remote_analyzer_acquire_call(
          self,
          SUSCAN_ANALYZER_REMOTE_SET_SPECTRUM_PARTITIONING),
      goto done);

  call->spectrum_partitioning = partitioning;

  SU_TRYCATCH(
      suscan_remote_analyzer_queue_call(self, call, SU_TRUE),
      goto done);

  ok = SU_TRUE;

done:
  if (call != NULL)
    suscan_remote_analyzer_release_call(self, call);

  return ok;
}

SUPRIVATE SUBOOL
suscan_remote_analyzer_set_hop_range(void *ptr, SUFREQ min, SUFREQ max)
{
  suscan_remote_analyzer_t *self = (suscan_remote_analyzer_t *) ptr;
  struct suscan_analyzer_remote_call *call = NULL;
  SUBOOL ok = SU_FALSE;

  SU_TRYCATCH(
      call = suscan_remote_analyzer_acquire_call(
          self,
          SUSCAN_ANALYZER_REMOTE_SET_HOP_RANGE),
      goto done);

  call->hop_range.min = min;
  call->hop_range.max = max;

  SU_TRYCATCH(
      suscan_remote_analyzer_queue_call(self, call, SU_TRUE),
      goto done);

  ok = SU_TRUE;

done:
  if (call != NULL)
    suscan_remote_analyzer_release_call(self, call);

  return ok;
}

SUPRIVATE SUBOOL
suscan_remote_analyzer_set_buffering_size(void *ptr, SUSCOUNT size)
{
  suscan_remote_analyzer_t *self = (suscan_remote_analyzer_t *) ptr;
  struct suscan_analyzer_remote_call *call = NULL;
  SUBOOL ok = SU_FALSE;

  SU_TRYCATCH(
      call = suscan_remote_analyzer_acquire_call(
          self,
          SUSCAN_ANALYZER_REMOTE_SET_BUFFERING_SIZE),
      goto done);

  call->buffering_size = size;

  SU_TRYCATCH(
      suscan_remote_analyzer_queue_call(self, call, SU_TRUE),
      goto done);

  ok = SU_TRUE;

done:
  if (call != NULL)
    suscan_remote_analyzer_release_call(self, call);

  return ok;
}

SUPRIVATE SUBOOL
suscan_remote_analyzer_write(void *ptr, uint32_t type, void *priv)
{
  suscan_remote_analyzer_t *self = (suscan_remote_analyzer_t *) ptr;
  struct suscan_analyzer_remote_call *call = NULL;
  SUBOOL ok = SU_FALSE;

  SU_TRYCATCH(
      call = suscan_remote_analyzer_acquire_call(
          self,
          SUSCAN_ANALYZER_REMOTE_MESSAGE),
      goto done);

  /* Transfer ownership */
  call->msg.type = type;
  call->msg.ptr  = priv;

  SU_TRYCATCH(
      suscan_remote_analyzer_queue_call(self, call, SU_TRUE),
      goto done);

  ok = SU_TRUE;

done:
  if (call != NULL)
    suscan_remote_analyzer_release_call(self, call);
  else
    suscan_analyzer_dispose_message(type, priv);

  return ok;
}

SUPRIVATE void
suscan_remote_analyzer_req_halt(void *ptr)
{
  suscan_remote_analyzer_t *self = (suscan_remote_analyzer_t *) ptr;
  struct suscan_analyzer_remote_call *call = NULL;

  SU_TRYCATCH(
      call = suscan_remote_analyzer_acquire_call(
          self,
          SUSCAN_ANALYZER_REMOTE_REQ_HALT),
      goto done);

  SU_TRYCATCH(
      suscan_remote_analyzer_queue_call(self, call, SU_TRUE),
      goto done);

done:
  if (call != NULL)
    suscan_remote_analyzer_release_call(self, call);
}

#define SET_CALLBACK(name) iface.name = JOIN(suscan_remote_analyzer_, name)

const struct suscan_analyzer_interface *
suscan_remote_analyzer_get_interface(void)
{
  static struct suscan_analyzer_interface iface;

  if (g_remote_analyzer_interface == NULL) {
    iface.name = "remote";

    SET_CALLBACK(ctor);
    SET_CALLBACK(dtor);
    SET_CALLBACK(set_frequency);
    SET_CALLBACK(set_gain);
    SET_CALLBACK(set_antenna);
    SET_CALLBACK(set_bandwidth);
    SET_CALLBACK(set_ppm);
    SET_CALLBACK(set_dc_remove);
    SET_CALLBACK(set_iq_reverse);
    SET_CALLBACK(set_agc);
    SET_CALLBACK(force_eos);
    SET_CALLBACK(is_real_time);
    SET_CALLBACK(get_samp_rate);
    SET_CALLBACK(get_measured_samp_rate);
    SET_CALLBACK(get_source_info_pointer);
    SET_CALLBACK(commit_source_info);
    SET_CALLBACK(set_sweep_strategy);
    SET_CALLBACK(set_spectrum_partitioning);
    SET_CALLBACK(set_hop_range);
    SET_CALLBACK(set_buffering_size);
    SET_CALLBACK(write);
    SET_CALLBACK(req_halt);

    g_remote_analyzer_interface = &iface;
  }

  return g_remote_analyzer_interface;
}

#undef SET_CALLBACK

