/*
Godnotic UI bridge: menu VM builtins (touch_uib_mvm.c). Included by mvm_cmds.c on one
line marked "// Touch:"; touch_uib_mvm.inc adds them to vm_m_builtins at #1180.
QuakeC declarations: qcsrc/menu/xonotic/touch_uib_api.qh.
*/
#ifndef TOUCH_UIB_MVM_H
#define TOUCH_UIB_MVM_H
struct prvm_prog_s;
void VM_uib_begin(struct prvm_prog_s *prog);
void VM_uib_node(struct prvm_prog_s *prog);
void VM_uib_end(struct prvm_prog_s *prog);
void VM_uib_done(struct prvm_prog_s *prog);
void VM_uib_s(struct prvm_prog_s *prog);
void VM_uib_f(struct prvm_prog_s *prog);
void VM_uib_b(struct prvm_prog_s *prog);
void VM_uib_j(struct prvm_prog_s *prog);
void VM_uib_push(struct prvm_prog_s *prog);
void VM_uib_q(struct prvm_prog_s *prog);
void VM_uib_nav(struct prvm_prog_s *prog);
void VM_uib_busy(struct prvm_prog_s *prog);
void VM_uib_toast(struct prvm_prog_s *prog);
void VM_uib_remove(struct prvm_prog_s *prog);
void VM_uib_rows_begin(struct prvm_prog_s *prog);
void VM_uib_row(struct prvm_prog_s *prog);
void VM_uib_rows_end(struct prvm_prog_s *prog);
void VM_uib_param(struct prvm_prog_s *prog);
void VM_uib_native(struct prvm_prog_s *prog);
void VM_uib_reset(struct prvm_prog_s *prog);
void VM_uib_rev(struct prvm_prog_s *prog);
void VM_uib_count(struct prvm_prog_s *prog);
void VM_uib_child(struct prvm_prog_s *prog);
void VM_uib_type(struct prvm_prog_s *prog);
void VM_uib_get(struct prvm_prog_s *prog);
void VM_uib_has(struct prvm_prog_s *prog);
void VM_uib_window(struct prvm_prog_s *prog);
void VM_uib_wtotal(struct prvm_prog_s *prog);
void VM_uib_wkey(struct prvm_prog_s *prog);
void VM_uib_wimage(struct prvm_prog_s *prog);
void VM_uib_wcell(struct prvm_prog_s *prog);
void VM_uib_imgpath(struct prvm_prog_s *prog);
void VM_uib_srcrev(struct prvm_prog_s *prog);
void VM_uib_modpages(struct prvm_prog_s *prog);
void VM_uib_caps(struct prvm_prog_s *prog);
void VM_uib_tick(struct prvm_prog_s *prog);
void VM_uib_nodegen(struct prvm_prog_s *prog);
void VM_uib_origin(struct prvm_prog_s *prog);
void VM_uib_event(struct prvm_prog_s *prog);
void VM_uib_qv(struct prvm_prog_s *prog);
void VM_uib_parent(struct prvm_prog_s *prog);
void VM_uib_loadinfo(struct prvm_prog_s *prog);
void VM_uib_thumb(struct prvm_prog_s *prog);
#endif
