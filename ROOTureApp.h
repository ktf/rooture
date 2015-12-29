#ifndef __ROOTURE_APP__
#define __ROOTURE_APP__

#include "TApplication.h"

struct lenv;
class TFileHandler;

class ROOTureApp : public TApplication {
public:
  ROOTureApp(int *argc, char **argv, lenv *e);
  virtual         ~ROOTureApp(); 
  virtual void    Run(Bool_t retrn = kFALSE);
  virtual Bool_t  HandleTermInput();
  virtual void    HandleException(Int_t sig);

  TFileHandler       *GetInputHandler() { return fInputHandler; }
  void        Interrupt() { fInterrupt = kTRUE; }
  
  ClassDef(ROOTureApp, 0);
private:
  ROOTureApp(const ROOTureApp&);               // not implemented
  ROOTureApp& operator=(const ROOTureApp&);    // not implemented

  lenv *fGlobalContext;
  TFileHandler *fInputHandler;
  Bool_t        fCaughtException;    // TRint just caught an exception or signal
  Bool_t        fInterrupt;          // if true macro execution will be stopped
};

#endif // __ROOTURE_APP__
