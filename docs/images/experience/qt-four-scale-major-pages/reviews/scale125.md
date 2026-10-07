# Independent125-r3 native major-page subset review

Actual source scope `/tmp/chat-daily-native-qt-scale125-r3-2y82nyss`. Personally viewed all24 original whole screenshots000–023 with original-detail image tool. No crop substituted for whole images; no supplementary crop was needed to identify these normal surfaces. Auxiliary `tui-image.png` is not one of24 captures. All24 local PNG SHA256 values independently match captures.json. No product/repo/SDK/build/test/SQL operation was performed by reviewer.

Actual counts: captures.json24; operations.json70 (JSON array of outcomes, not70 independent screenshot tests): capture21, click20, field4, finish1, keys14, qt-text1, resize3, wait-qt6. Automatic initial login captures and final handoff account for other3 captures. No operation type containing `fail` or top-level recorded error. Root reports observed launcher exit0; no driver.exit file exists within copied scope, so this review does not invent one. cleanup.status remains `HANDOFF_NOT_PASS`, appropriately not an automatic product-PASS claim.

## Actual125% coordinate correction

Public tree002 Chat rect `[0,0,980,640]` is logical, raw xwininfo002 actual `1225x800+0+0` physical. Real contact click result is logical rect `[9,136,64,64]`, physical point `[51,210]`, scale1.25;003 actually shows Contacts, unlike failed previousr2. Actual requests/newfriends click point310,94 reaches004 requests. Actual group header744,30 opens008 group modal; tab731,44 selects011 Management.20 click results all record physical_click and1.25, but do not assert every OS-level coordinate space uses this mapping universally.

Raw xwininfo008 actual group650×850+400+0, main1475×950+0+0, consistent with tree group520×680. Profile014650×734+400+59 is consistent with logical520×587 rounding. Naming019650×375+400+88 is logical520×300. Whole originals show correctly placed corresponding windows and controls, no detached click target or product-layout scaling failure identified.

## All original visual checkpoints

| Captures | Actual visible content and bounded conclusion |
|---|---|
| 000 | Compact login card/brand/username/password/login/register/right gear intact at125%. Collapsed server field only; no register/error path. |
| 001 / 002 | Default chat and actual logical980×640 main window: sidebars, header, text bubble, composer/file/send within frame. No overlapping/cut control. Short history only. |
| 003 / 004 / 005 | Contacts really displayed, then distinct incomingD/outgoingE request sections; logical1920×1080 resize expands main surface. These PNGs are not duplicates of002. Rows/count labels/focus outline readable and uncut; no long/paged list coverage. |
| 006 / 007 | AddFriend blank prompt then true returned申请D row, not merely正在搜索. Search field/result are inside sidebar. No native add/accept persisted effect claimed. |
| 008 / 009 | Owner overview520×680 with short announcement/3 preview rows and complete controls, then all4 member rows and End-focusS005. Public names include role/self in tree. No large-20 member scroll or peer profile. |
| 010 / 011 | One real pending request plus through/reject/Close controls; Management title/announcement/approval/invitation/create-copy-revoke all fit regular dialog. Announcement has keyboard focus outline. Clean saves correctly disabled; no minimum420×400, long announcement, dirty save or revoked-role branch. |
| 012 / 013 | Search blank validation prompt then one actual matched text with keyboard outline. Search/action/helper/bottom buttons fit. No more-page, no result-open feature, no context Copy effect. |
| 014 / 015 | Own account profile actual520×587; avatar/name/copy/avatar-change/logout and36px close all fit; nested logout confirmation has clear action and borderedCancel. Actual Escape follows015 in operations;020/023 prove later app still authenticated. No confirmed logout or incoming/accepted-peer profile here. |
| 016 | Actual3-row ChatActions popup is readable/unclipped. Public menu/menuitem bridge absence remains a limitation; native keyboard outcome proves subsequent dialogs instead. Home shortcut is NOT reused or asserted. |
| 017 | True CreateGroup first page,0 selected and actualNext disabled. Two existing accepted contacts listed; no fabricated3 contact seed. |
| 018 | True C checkmark and selected tint, selected chip/count1, Next green. Tree C checked=true, selected=true, focused=true; S005 checked=false; Next enabled=true. This is not click delivery alone. |
| 019 / 020 | True naming page selected1, complete44px C row, title/actions visible.019 has member-list keyboard outline;020 after back/next has title-focus outline. Both trees actual Text群名称 exactly `  页面矩阵 中文 é 🙂  ` (two leading/trailing spaces, decomposed e+U0301). The explicit qt-text outcome equals same string. No create clicked, no new-group SQL persistence asserted. |
| 021 / 022 | True JoinGroup small modal, label/input/Join/Cancel intact.022 contains invalid string but no visible invalid-link feedback: input entry only, not submitted validation branch. |
| 023 | Main chat after join cancellation; no leftover modal, controls/message intact. Final handoff not business completion. |

No high-confidence material clipping/overlap/layout defect found in this major-page125% subset. Visible focus/check/selection changes are real; global keyboard reachability, allstate/Orca/AT-SPI notification correctness are not established. Regular group/dialog widths are visually coherent; short title and single-line announcement deliberately do not test overflow stress. Most group/modal capture is over restored1180×760 logical main, not980×640.

## Provenance / cleanup / limits

Freeze records clean HEAD7375815, scale1.25, current QtSHA370a68ae9448916c86e3085f1a459e9b1ea0f2d292cf18113c842de96a1b24a4. Current69=false; normal23=112.59 s/root observed0, no ASAN/UBSAN or original verify rerun. Recorded capture a11y_error_count all0 and a11y-errors.json `[]`: only this logger's recorded errors, not proof all possible bridge event/Orca errors0.

Cleanup10 owned records and5 activation-descendant records all gone=true; protected_unchanged=true; errors[]; tmux unused null. Qt and final clipboard -15 are intentional teardown statuses, not product crashes. Database explicitly retained/present for evidence, not falsely deleted. Driverexit0 as reported by root does not convert retainedDB/hand-off status into full product PASS.

Previous125r2 logicalclick failure remains independent unsuccessful driver evidence, not overwritten or counted here.100% initial false create labels remain unsuccessful too. This review neither modifies those records nor extends24 screenshots to fourDPI/full state coverage. Missing editor/reply/reaction/read/attachment/nativechoosers, registration/server reveal, trueaccept/invite/newgroup create, privateordinarygroup, transport/reconnect and successful logout are unchanged gaps unless separate actual records establish them.
