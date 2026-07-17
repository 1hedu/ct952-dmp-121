/*
 * JupiterSDK on CT952 -- demo application, firmware integration surface.
 *
 * Integrates into the DVD firmware's cooperative superloop exactly like
 * the built-in OSD games (gamemain.h): a ProcessKey handler in the
 * FuncArray dispatch table and a Trigger called from CC_DVD_MainLoop
 * while the app owns the screen. Enabled by SUPPORT_JUPITER.
 *
 * Entry: the GAME key on the remote (KEY_OSDGAME -- free in this build
 * because SUPPORT_OSDGAME is compiled out). Exit: KEY_STOP or the GAME
 * key again.
 */
#ifndef JAPP_H
#define JAPP_H

/* Key handler for the FuncArray dispatch table. Returns KEY_BYPASS when
 * the Jupiter app is inactive and the key is not ours; otherwise
 * consumes the key and returns KEY_NO_KEY. */
BYTE JUPITER_ProcessKey(void);

/* Nonzero while the Jupiter app owns the screen (checked by the
 * superloop the same way __enGameID gates GAMEMAIN_Trigger). */
BYTE JUPITER_IsActive(void);

/* Per-iteration tick from CC_DVD_MainLoop while active: paces and
 * renders one frame, never blocks. */
void JUPITER_Trigger(void);

#endif /* JAPP_H */
