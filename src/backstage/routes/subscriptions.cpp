// POST /subscriptions
// Create a Backstage subscription for live object, log, or diagnostic updates.

static ERR post_subscriptions(const BackstageRequest &Request, BackstageResponse &Response)
{
   return ERR::Okay;
}

// GET /subscriptions/{id}
// Return meta data and current state for a Backstage subscription.

static ERR get_subscriptions_id(const BackstageRequest &Request, BackstageResponse &Response)
{
   return ERR::Okay;
}

// DELETE /subscriptions/{id}
// Delete a Backstage subscription.

static ERR delete_subscriptions_id(const BackstageRequest &Request, BackstageResponse &Response)
{
   return ERR::Okay;
}
