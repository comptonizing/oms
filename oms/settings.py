import json
import logging

logger = logging.getLogger(__name__)

__settings = None

def get():
    global __settings
    if __settings is None:
        raise ValueError("Settings not initialized")
    return __settings

def load(fname):
    logger.info("Loading settings from {}".format(fname))
    global __settings
    with open(fname) as f:
        __settings = json.load(f)
    logger.debug("Settings: {}". format(json.dumps(__settings, indent=4)))

def query(*args):
    logger.debug("Query: {}".format(args))
    thing = get()
    for key in args:
        thing = thing[key]
    logger.debug("Result: {}".format(thing))
    return thing

def q(*args):
    return query(*args)

def dump(**kwargs):
    logger.debug("Settings dump")
    settings = get()
    d = json.dumps(settings, **kwargs)
    return d
