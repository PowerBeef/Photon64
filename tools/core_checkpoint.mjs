// Seed equivalent software/GPU comparisons from one software warm-up.
// Both instances must use the same core and cartridge; static pointers are
// identical, while the external ROM allocation remains owned by each instance.
export function seedSoftwareCore(source, target, romSize) {
  const size = source.ex.n64_state_size();
  if (size !== target.ex.n64_state_size()) throw new Error('Checkpoint core layout differs');
  new Uint8Array(target.ex.memory.buffer, 0, size).set(new Uint8Array(source.ex.memory.buffer, 0, size));
  target.ex.n64_set_rom(target.ptr, romSize);
}
