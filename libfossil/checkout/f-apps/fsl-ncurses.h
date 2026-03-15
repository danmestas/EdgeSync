#if !defined(FSL_NCURSES_INCLUDED)
#define FSL_NCURSES_INCLUDED
#include <ncurses.h>
#include "libfossil.h"

/**
   If the ncurses has not yet been initialized, this function does so,
   else it is a no-op. When it initializes the screen, it sets up
   default values for the color pairs described by the
   fsl_dibu_nc_attr_e enum. Clients may redefine those if they wish,
   as documented for that enum.

   If this function is called before ncurses has initialized the
   screen, it also calls `setlocale(LC_ALL,"")` so that curses can
   render multi-byte UTF8 characters.

   This function counts how many times it is called. See
   fnc_screen_shutdown() for why.

   Potential TODO:

   - Add an argument which specifies a general color theme to use,
     e.g. "light" vs "dark".
*/
void fnc_screen_init(void);

/**
   If the ncurses has been initialized via fnc_screen_init() AND that
   function has been called the same number of times as this one, this
   function shuts down ncurses mode, else it is a no-op.
*/
void fnc_screen_shutdown(void);

/**
   Curses attribute references used by the fsl_dibu_nc diff renderer.
   These are indexes which may be passed to...

   @see fsl_dibu_nc_attr_get()
   @see fsl_dibu_nc_attr_set()
   @see fsl_dibu_ncu_alloc()
*/
enum fsl_dibu_nc_attr_e {
/**
   The API guarantees that it will not define any COLOR_PAIR() entries
   with indexes less than this value.
*/
fsl_dibu_nc_attr__start = 16,
/** Attributes for the diff window. */
fsl_dibu_nc_attr_Window = fsl_dibu_nc_attr__start,
/** Attributes for the "Index" lines. */
fsl_dibu_nc_attr_Index,
/** Attributes for the "chunk splitter" lines. */
fsl_dibu_nc_attr_ChunkSplitter,
/** Attributes for the common lines. */
fsl_dibu_nc_attr_Common,
/** Attributes for the inserted (RHS-only) lines. */
fsl_dibu_nc_attr_Insert,
/** Attributes for the deleted (LHS-only) lines. */
fsl_dibu_nc_attr_Delete,
/** Attributes for the diff view mini-help. */
fsl_dibu_nc_attr_Help,
/** Attributes for the end-of-diff line. */
fsl_dibu_nc_attr_EOF,
/** End-of-enum sentinel, used for size calculations. */
fsl_dibu_nc_attr__end
};
typedef enum fsl_dibu_nc_attr_e fsl_dibu_nc_attr_e;
/**
   Returns the curses attributes defined for the given enum
   value. Calling this before fnc_screen_init() has been called
   leads to undefined results.
*/
unsigned int fsl_dibu_nc_attr_get(fsl_dibu_nc_attr_e n);
/**
   Sets the curses attributes defined for the given enum value. The
   library sets these up to default values when fnc_screen_init() is
   called.
*/
void fsl_dibu_nc_attr_set(fsl_dibu_nc_attr_e n, unsigned int v);

/**
   Factory function for the "ncu" (ncurses unified) diff builder. It
   must eventually be freed via its own finalize() method.

   This diff builder has the following peculiarities:

   - The first time its start() method is run, it initializes the
     ncurses screen state via fnc_screen_init().

   - Its finally() method will enter an interactive loop and display
     the generated diff until the close-diff key is activated (it will
     be shown on the screen). After finally() has been called, it must
     not be used for further diff processing. If needed again,
     finalize() this one and allocate a new one.

   - Its finalize() method runs fnc_screen_shutdown(). Because the
     fnc_screen_init() API counts how many times it is called, it will
     not actually shut down the ncurses screen if the client app calls
     fnc_screen_init() before the builder's start() method is called.
*/
fsl_dibu * fsl_dibu_ncu_alloc(void);

/**
   Renders a vertical scrollbar on window tgt at column x, running
   from the given top row to the row (top+barHeight). The scroll
   indicator is rendered depending on the final two arguments:
   lineCount specifies the number of lines in the scrollable
   input and currentLine specifies the top-most line number of the
   currently-displayed data. e.g. a lineCount of 100 and currentLine
   of 1 puts the indicator 1% of the way down the scrollbar. Likewise,
   a lineCount of 100 and currentLine of 90 puts the indicator at the
   90% point.
*/
void fnc_scrollbar_v( WINDOW * const tgt, int top, int x, 
                      int barHeight, int lineCount,
                      int currentLine );

/**
   Works like fnc_scrollbar_v() but renders a horizontal scrollbar
   at the given top/left coordinates of tgt.
*/
void fnc_scrollbar_h( WINDOW * const tgt, int top, int left, 
                      int barWidth, int colCount,
                      int currentCol );

#endif /* FSL_NCURSES_INCLUDED */
