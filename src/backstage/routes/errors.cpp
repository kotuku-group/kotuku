// GET /errors
// Return a JSON list of known error codes and their symbolic names.

static ERR get_errors(const BackstageRequest &Request, BackstageResponse &Response)
{
   return ERR::Okay;
}

// GET /errors/{code}
// Return meta data for a single error code.

static ERR get_errors_code(const BackstageRequest &Request, BackstageResponse &Response)
{
   return ERR::Okay;
}

