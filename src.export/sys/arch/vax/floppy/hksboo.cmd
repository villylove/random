!	$NetBSD: hksboo.cmd,v 1.2 1998/01/05 20:52:09 perry Exp $
!
! BOOTSTRAP ON HK, LEAVING SINGLE USER
!
SET DEF HEX
SET DEF LONG
SET REL:0
HALT
UNJAM
INIT
LOAD BOOT
D R10 3		! DEVICE CHOICE 3=HK
D R11 2		! 2= RB_SINGLE
START 2