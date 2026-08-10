import json
import logging

from nicegui import app, ui

logger = logging.getLogger(__name__)

def query(thing, **kwargs):
    """Reads a setting. Anything the user has not configured is empty and reads as None.

    There are no built-in defaults: every setting is entered by the operator, and until
    then it has no value. A stored None counts as unconfigured as well, rather than as a
    value -- an emptied ui.number binds None, and put() unsets instead of storing it, so
    this also covers storage written before that was true.

    A caller that structurally needs a value passes fallback=. A caller without one has
    to cope with None, which for nearly all of them means an explicit "not configured"
    path; the availability-reason functions in oms/oms exist for exactly that.
    """
    logger.debug("Query: {}".format(thing))
    ret = app.storage.general.get(thing)
    if ret is not None:
        logger.debug("Result: {}".format(ret))
        return ret
    if "fallback" in kwargs:
        logger.debug("Returning fallback {}".format(kwargs["fallback"]))
        return kwargs["fallback"]
    logger.debug("{} is not configured".format(thing))
    return None

def q(*args, **kwargs):
    return query(*args, **kwargs)

def put(thing, value):
    """Writes a setting. Clearing a field unsets it rather than storing None.

    Storing None would leave the key present but holding a non-value. Unsetting it makes
    an emptied field behave exactly like one that was never filled in, which is the whole
    contract: unconfigured is unconfigured, however it got that way.
    """
    if value is None:
        logger.debug("Unset: {} (value is None)".format(thing))
        rm(thing)
        return
    logger.debug("Set: {}: {}".format(thing, value))
    app.storage.general[thing] = value

def p(thing, value):
    return put(thing, value)

def dump(**kwargs):
    logger.debug("Settings dump")
    return json.dumps(dict(app.storage.general), **kwargs)

def has(thing):
    # None means "not configured", the same as an absent key -- callers use has() to
    # decide whether querying without a fallback is safe (see readRoofConfig).
    return app.storage.general.get(thing) is not None

def rm(thing):
    if thing in app.storage.general:
        del app.storage.general[thing]
