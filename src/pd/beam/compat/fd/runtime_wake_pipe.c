#include "runtime_wake_pipe.h"

static runtime_pipe_slot *slot_of_read(runtime_pipe_slot *table, size_t count,
                                       int fd) {
  for (size_t i = 0; i < count; i++) {
    if (table[i].state != runtime_pipe_free && table[i].read_open &&
        table[i].read_fd == fd) {
      return &table[i];
    }
  }
  return nullptr;
}

static runtime_pipe_slot *slot_of_write(runtime_pipe_slot *table, size_t count,
                                        int fd) {
  for (size_t i = 0; i < count; i++) {
    if (table[i].state != runtime_pipe_free && table[i].write_open &&
        table[i].write_fd == fd) {
      return &table[i];
    }
  }
  return nullptr;
}

runtime_pipe_status runtime_pipe_open(runtime_pipe_slot *table, size_t count,
                                      int read_fd, int write_fd) {
  for (size_t i = 0; i < count; i++) {
    if (table[i].state == runtime_pipe_free) {
      table[i] = (runtime_pipe_slot){.state = runtime_pipe_empty,
                                     .read_open = true,
                                     .write_open = true,
                                     .read_fd = read_fd,
                                     .write_fd = write_fd};
      return runtime_pipe_ok;
    }
  }
  return runtime_pipe_no_slot;
}

runtime_pipe_status runtime_pipe_write(runtime_pipe_slot *table, size_t count,
                                       int fd, size_t bytes, size_t *accepted) {
  *accepted = 0;
  runtime_pipe_slot *slot = slot_of_write(table, count, fd);
  if (slot == nullptr) {
    return runtime_pipe_unknown_fd;
  }
  if (!slot->read_open) {
    return runtime_pipe_broken;
  }
  const uint32_t room = runtime_pipe_capacity - slot->pending;
  if (bytes != 0 && room == 0) {
    return runtime_pipe_would_block;
  }
  const uint32_t take = bytes < room ? (uint32_t)bytes : room;
  slot->pending += take;
  if (slot->pending != 0) {
    slot->state = runtime_pipe_readable;
  }
  *accepted = take;
  return runtime_pipe_ok;
}

runtime_pipe_status runtime_pipe_read(runtime_pipe_slot *table, size_t count,
                                      int fd, size_t bytes, size_t *taken) {
  *taken = 0;
  runtime_pipe_slot *slot = slot_of_read(table, count, fd);
  if (slot == nullptr) {
    return runtime_pipe_unknown_fd;
  }
  if (slot->state == runtime_pipe_empty) {
    return slot->write_open ? runtime_pipe_would_block : runtime_pipe_eof;
  }
  const uint32_t take = bytes < slot->pending ? (uint32_t)bytes : slot->pending;
  slot->pending -= take;
  if (slot->pending == 0) {
    slot->state = runtime_pipe_empty;
  }
  *taken = take;
  return runtime_pipe_ok;
}

bool runtime_pipe_ready(const runtime_pipe_slot *table, size_t count, int fd) {
  for (size_t i = 0; i < count; i++) {
    const runtime_pipe_slot *slot = &table[i];
    if (slot->state != runtime_pipe_free && slot->read_open &&
        slot->read_fd == fd) {
      return slot->state == runtime_pipe_readable || !slot->write_open;
    }
  }
  return false;
}

runtime_pipe_status runtime_pipe_close(runtime_pipe_slot *table, size_t count,
                                       int fd) {
  runtime_pipe_slot *slot = slot_of_read(table, count, fd);
  if (slot != nullptr) {
    slot->read_open = false;
  } else if ((slot = slot_of_write(table, count, fd)) != nullptr) {
    slot->write_open = false;
  } else {
    return runtime_pipe_unknown_fd;
  }
  if (!slot->read_open && !slot->write_open) {
    *slot = (runtime_pipe_slot){.state = runtime_pipe_free};
  }
  return runtime_pipe_ok;
}
