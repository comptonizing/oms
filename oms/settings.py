import json
import logging

from nicegui import app, ui

logger = logging.getLogger(__name__)

DEFAULTS = dict(
        stream_rtsp = "",
        stream_refresh = 10,
        weather_refresh = 10,
        weather_elevation = 0,
        weather_image = "",
        weather_url = "",
        weather_db_url = "",
        weather_db_org = "",
        weather_db_token = "",
        weather_db_bucket = "",
        roof_port = "",
        roof_pins_west_open = 3,
        roof_pins_west_close = 17,
        roof_pins_east_open = 4,
        roof_pins_east_close = 23,
        switches = {},
        )

async def init():
    for key, value in DEFAULTS.items():
        app.storage.general.setdefault(key, value)

def query(thing):
    logger.debug("Query: {}".format(thing))
    ret = app.storage.general[thing]
    logger.debug("Result: {}".format(ret))
    return ret

def q(*args):
    return query(*args)

def put(thing, value):
    logger.debug("Set: {}: {}".format(thing, value))
    app.storage.general[thing] = value

def p(thing, value):
    return put(thing, value)

def dump(**kwargs):
    logger.debug("Settings dump")
    return json.dumps(dict(app.storage.general), **kwargs)
