; ─────────────────────────────────────────────────────────────────────────────
; Ainoiceguard NSIS include — VB-Cable post-install offer
;
; WHAT THIS DOES:
;   After Ainoiceguard finishes installing, this optionally runs the
;   VB-Cable installer silently (requires VB-Cable bundle to be present).
;
; HOW TO ENABLE:
;   1. Contact VB-Audio for redistribution permission:
;      https://vb-audio.com/Services/contact.htm
;   2. Download VBCABLE_Driver_Pack45.zip from vb-audio.com and extract
;      VBCABLE_Setup_x64.exe into: build/installer/vendor/
;   3. Set env var VB_CABLE_ENABLED=1 before building.
;
;   Until then, the include does nothing — no pages shown, no installs run.
;
; LICENSE NOTE:
;   Per VB-Audio license: redistribution requires written Author agreement.
;   VB_CABLE_ENABLED=0 prevents accidental inclusion until licensed.
; ─────────────────────────────────────────────────────────────────────────────

!if VB_CABLE_ENABLED

; ── Run VB-Cable installer silently after Ainoiceguard is installed ─────────
!macro customInstall
  ; VB-Cable /S = silent install (no UI). Reboot is requested automatically.
  ExecWait '"build\installer\vendor\VBCABLE_Setup_x64.exe" /S'
!macroend

!endif