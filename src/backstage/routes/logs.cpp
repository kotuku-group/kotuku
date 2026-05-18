// POST /logs/level
// Change runtime logging levels for Backstage or another module.

static ERR post_logs_level(const BackstageRequest &Request, BackstageResponse &Response)
{
   return ERR::Okay;
}

// POST /logs/start
// Activates internal log recording.

static ERR post_logs_start(const BackstageRequest &Request, BackstageResponse &Response)
{
   return ERR::Okay;
}

// POST /logs/stop
// Stops internal log recording.

static ERR post_logs_stop(const BackstageRequest &Request, BackstageResponse &Response)
{
   return ERR::Okay;
}

// GET /logs
// Returns all logged messages, then clears the log message stack (the client is responsible for maintaining a permanent record).

static ERR get_logs(const BackstageRequest &Request, BackstageResponse &Response)
{
   return ERR::Okay;
}
