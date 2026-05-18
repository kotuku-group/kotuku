// GET /ping
// Return a small JSON response to confirm that Backstage can receive HTTP requests and send responses.

static ERR get_ping(const BackstageRequest &Request, BackstageResponse &Response)
{
   return ERR::Okay;
}

