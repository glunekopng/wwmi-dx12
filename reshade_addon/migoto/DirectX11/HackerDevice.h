// Forwarding header. Their sources include "DirectX11\HackerDevice.h" by path; ours is the SHIM in migoto\,
// not their full D3D11 wrapper -- see docs/PORT-AUDIT.md for why the wrapper is not carried across.
// This exists so their path-qualified include resolves without editing their source.
#pragma once
#include "../HackerDevice.h"
