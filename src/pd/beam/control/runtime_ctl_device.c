/* Table-driven: gate[] is the transition table from the header. An admitted
 * event runs its action, which applies the record guards and sets the next
 * phase. Records are copied with memcpy because the caller's buffer has no
 * alignment guarantee. */
#include "runtime_ctl_device.h"

#include <string.h>

static_assert(sizeof(chryso_ctl_command) == sizeof(chryso_ctl_reply));
static_assert(sizeof((chryso_ctl_command){}.args) ==
              runtime_ctl_words * sizeof(uint64_t));
static_assert(chryso_abi_version <= UINT16_MAX);

typedef enum : uint8_t {
  event_open,
  event_write,
  event_reply,
  event_read,
  event_close,
  event_count,
} event;

static constexpr size_t phase_count = runtime_ctl_reply_ready + 1;

/* ok admits the event; anything else is the rejection, state kept. */
static const runtime_ctl_status gate[phase_count][event_count] = {
    [runtime_ctl_closed] =
        {
            [event_open] = runtime_ctl_ok,
            [event_write] = runtime_ctl_not_owner,
            [event_reply] = runtime_ctl_not_owner,
            [event_read] = runtime_ctl_not_owner,
            [event_close] = runtime_ctl_not_owner,
        },
    [runtime_ctl_idle] =
        {
            [event_open] = runtime_ctl_busy,
            [event_write] = runtime_ctl_ok,
            [event_reply] = runtime_ctl_out_of_order,
            [event_read] = runtime_ctl_no_reply,
            [event_close] = runtime_ctl_ok,
        },
    [runtime_ctl_calling] =
        {
            [event_open] = runtime_ctl_busy,
            [event_write] = runtime_ctl_busy,
            [event_reply] = runtime_ctl_ok,
            [event_read] = runtime_ctl_busy,
            [event_close] = runtime_ctl_ok,
        },
    [runtime_ctl_reply_ready] =
        {
            [event_open] = runtime_ctl_busy,
            [event_write] = runtime_ctl_busy,
            [event_reply] = runtime_ctl_out_of_order,
            [event_read] = runtime_ctl_ok,
            [event_close] = runtime_ctl_ok,
        },
};

/* Ownership first: outside closed, only the owner's fd may act, and open
 * is the one event that names a new fd. */
static runtime_ctl_status admit(const runtime_ctl_owner *owner, int fd,
                                event ev) {
  if (owner->phase >= phase_count) {
    return runtime_ctl_out_of_order;
  }
  if (owner->phase != runtime_ctl_closed && ev != event_open &&
      owner->fd != fd) {
    return runtime_ctl_not_owner;
  }
  return gate[owner->phase][ev];
}

runtime_ctl_status runtime_ctl_open(runtime_ctl_owner *owner, int fd) {
  const runtime_ctl_status admitted = admit(owner, fd, event_open);
  if (admitted != runtime_ctl_ok) {
    return admitted;
  }
  *owner = (runtime_ctl_owner){.phase = runtime_ctl_idle, .fd = fd};
  return runtime_ctl_ok;
}

runtime_ctl_status runtime_ctl_write(runtime_ctl_owner *owner, int fd,
                                     const void *buf, size_t count,
                                     runtime_ctl_call *call) {
  const runtime_ctl_status admitted = admit(owner, fd, event_write);
  if (admitted != runtime_ctl_ok) {
    return admitted;
  }
  if (buf == nullptr || count != sizeof(chryso_ctl_command)) {
    return runtime_ctl_size;
  }
  if (chryso_check_ctl_command(buf, count) != chryso_abi_reject_ok) {
    return runtime_ctl_malformed;
  }
  chryso_ctl_command command = {};
  memcpy(&command, buf, sizeof(command));
  *call = (runtime_ctl_call){.opcode = command.opcode};
  memcpy(call->args, command.args, sizeof(call->args));
  owner->phase = runtime_ctl_calling;
  return runtime_ctl_ok;
}

/* Root answers with a declared nonzero CtlResult in four words. Label 0 is
 * also what Microkit returns for an undeclared PPC id. */
static bool reply_frame_valid(uint64_t label, uint64_t count) {
  return count == runtime_ctl_words && label != 0 && label <= UINT16_MAX &&
         chryso_ctl_result_is_member((uint16_t)label);
}

runtime_ctl_status
runtime_ctl_reply(runtime_ctl_owner *owner, int fd, uint64_t label,
                  uint64_t count,
                  const uint64_t values[static runtime_ctl_words]) {
  const runtime_ctl_status admitted = admit(owner, fd, event_reply);
  if (admitted != runtime_ctl_ok) {
    return admitted;
  }
  if (!reply_frame_valid(label, count)) {
    owner->phase = runtime_ctl_idle;
    return runtime_ctl_unavailable;
  }
  /* reply_frame_valid bounded label to 16 bits. */
  owner->reply = (chryso_ctl_reply){
      .magic = chryso_magic_reply,
      .version = (uint16_t)chryso_abi_version,
      .result = (uint16_t)label,
  };
  memcpy(owner->reply.values, values, sizeof(owner->reply.values));
  owner->phase = runtime_ctl_reply_ready;
  return runtime_ctl_ok;
}

runtime_ctl_status runtime_ctl_read(runtime_ctl_owner *owner, int fd, void *buf,
                                    size_t count) {
  const runtime_ctl_status admitted = admit(owner, fd, event_read);
  if (admitted != runtime_ctl_ok) {
    return admitted;
  }
  if (buf == nullptr || count != sizeof(chryso_ctl_reply)) {
    return runtime_ctl_size;
  }
  memcpy(buf, &owner->reply, sizeof(owner->reply));
  owner->reply = (chryso_ctl_reply){};
  owner->phase = runtime_ctl_idle;
  return runtime_ctl_ok;
}

runtime_ctl_status runtime_ctl_close(runtime_ctl_owner *owner, int fd) {
  const runtime_ctl_status admitted = admit(owner, fd, event_close);
  if (admitted != runtime_ctl_ok) {
    return admitted;
  }
  *owner = (runtime_ctl_owner){};
  return runtime_ctl_ok;
}

runtime_stat_step runtime_stat_read_decide(size_t count, uint64_t position,
                                           size_t page_size) {
  if (position != 0) {
    return runtime_stat_eof;
  }
  return count < page_size ? runtime_stat_short : runtime_stat_copy;
}
