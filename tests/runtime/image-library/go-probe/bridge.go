//go:build cgo

package main

/*
#include <stdint.h>
*/
import "C"

//export ImageLibraryProbe
func ImageLibraryProbe() C.int {
	return C.int(runSelfTest())
}
