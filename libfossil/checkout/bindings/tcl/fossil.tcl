# This file is intended to be sourced by libfossil's pkgIndex.tcl.
# It currently has nothing useful to do.
if {[info exists dir]} {
  #puts "dir=[file normalize $dir]"
}

return
# What follows is an experiment which may (or may not) end up being
# compiled in to the extension. Running this may break certain tests.
namespace eval f {

  proc ls {args} {
    set xargs {-size -time}
    set header {UUID           P  Size      Date                Name}
    set single 0
    set checkedFmt 0
    foreach e [fossil ls {*}$args] {
      #puts "e=$e"
      if {!$checkedFmt} {
        incr checkedFmt
        set single [expr [llength $e] == 1]
      }
      if {$single} {
        puts $e
      } else {
        array set f $e
        set line [format %-16s [string range $f(uuid)]]
        if {"" eq $f(perm)} {
          append line " - "
        } else {
          append line " " $f(perm) " "
        }
        if {$more} {
          append line [format %12s $f(size)]
          append line [format %20s $f(time)]
        }
        append line $f(name)
        puts $line
      }
    }
  }

  namespace export ls
  namespace ensemble create
}; # namespace f
