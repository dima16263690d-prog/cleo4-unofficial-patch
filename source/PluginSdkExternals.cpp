#include "stdafx.h"
#include "CVector.h"
#include "CRGBA.h"
#include "CPed.h"

bool CPed::IsPlayer() { return m_nPedType == PED_TYPE_PLAYER1 || m_nPedType == PED_TYPE_PLAYER2; }
CRGBA::CRGBA(unsigned char _r, unsigned char _g, unsigned char _b, unsigned char _a) : r(_r), g(_g), b(_b), a(_a) { }
CRGBA &CRGBA::operator=(const CRGBA &rgba) { r = rgba.r; g = rgba.g; b = rgba.b; a = rgba.a; return *this; }