// GET /objects
// Return a JSON list of all objects and their basic meta data.

static ERR get_objects(const BackstageRequest &Request, BackstageResponse &Response)
{
   return ERR::Okay;
}

// POST /objects
// Create a new object, using the provided JSON fields as the object field values

static ERR post_objects(const BackstageRequest &Request, std::string_view Body, BackstageResponse &Response)
{
   return ERR::Okay;
}

// GET /objects/{uid}
// Get a list of all readable field values of the target object.

static ERR get_objects_uid(const BackstageRequest &Request, BackstageResponse &Response)
{
   return ERR::Okay;
}

// GET /objects/{uid}/children
// Return the child objects owned by the target object.

static ERR get_objects_uid_children(const BackstageRequest &Request, BackstageResponse &Response)
{
   return ERR::Okay;
}

// GET /objects/{uid}/subscribers
// Return subscriptions and callbacks associated with the target object.

static ERR get_objects_uid_subscribers(const BackstageRequest &Request, BackstageResponse &Response)
{
   return ERR::Okay;
}

// POST /objects/{uid}
// Process a series of commands (e.g. call an action, set a field) for the target object.  Returns an error code and any result values.

static ERR post_objects_uid(const BackstageRequest &Request, std::string_view Body, BackstageResponse &Response)
{
   return ERR::Okay;
}

