// GET /ping
// Return a small JSON response to confirm that Backstage can receive HTTP requests and send responses.

static ERR get_ping(const BackstageRequest &Request, BackstageResponse &Response)
{
   Response.status = 200;
   Response.body = "{\"status\":\"pong\"}";
   Response.contentType = "application/json";
   return ERR::Okay;
}
