//********************************************************************************************************************
// @doc POST /logs/level Change the logging level for the program (affects console output).

static ERR post_logs_level(const BackstageRequest &Request, BackstageResponse &Response)
{
   return ERR::NoSupport;
}

//********************************************************************************************************************
// @doc POST /logs/start Activates internal log recording.

static ERR post_logs_start(const BackstageRequest &Request, BackstageResponse &Response)
{
   return ERR::NoSupport;
}

//********************************************************************************************************************
// @doc POST /logs/stop Stops internal log recording.

static ERR post_logs_stop(const BackstageRequest &Request, BackstageResponse &Response)
{
   return ERR::NoSupport;
}

//********************************************************************************************************************
// @doc GET /logs Returns all logged messages, then clears the log message stack (the client is responsible for maintaining a permanent record).

static ERR get_logs(const BackstageRequest &Request, BackstageResponse &Response)
{
   return ERR::NoSupport;
}

