from nicegui import app
from http import HTTPStatus as status
from fastapi.responses import JSONResponse, Response
import requests
import logging

import settings as s

logger = logging.getLogger(__name__)

__base = "/api/v1/"

# How long a scrape of the weather station may take. Named rather than inline because
# oms/oms derives the weather worker's stop budget from it: the worker sits inside this
# call, so a join that has to outlast a scrape has to be sized against this number, and
# the two must not be able to drift apart. Note requests applies it per phase -- connect
# and read get one each -- so the worst case is twice this, and DNS resolution is not
# covered by it at all.
WEATHER_HTTP_TIMEOUT = 10

@app.get(__base + "id")
def id():
    logger.debug("id endpoint call")
    return Response(content="OMS API v1", media_type='text/plain')

@app.get(__base + "weather")
def weather():
    logger.debug("weather endpoint call")
    url = s.q("weather_url")
    if url in [None, ""]:
        logger.warning("Weather scraping URL is not configured")
        return JSONResponse(
                content = {"Error": "No weather URL is configured"},
                status_code = status.SERVICE_UNAVAILABLE
                )
    try:
        logger.debug("Getting weather from {}".format(url))
        r = requests.get(url, timeout=WEATHER_HTTP_TIMEOUT)
    except Exception as e:
        logger.error("Error getting weather: {}".format(e))
        return JSONResponse(
                content = {"Error": "Could not send request"},
                status_code = status.SERVICE_UNAVAILABLE
                )

    if r.status_code != status.OK:
        logger.error("Error getting weather: Got status {} from endpoint, message: {}".format(r.status_code, r.content))
        return JSONResponse(
                content = {
                    "Error": "Got HTTP code {} from endpoint".format(r.status_code),
                    "Status": r.status_code,
                    "Content": r.text
                    },
                status_code = status.SERVICE_UNAVAILABLE
                )
    logger.debug("Response: {}".format(r.text))
    return Response(
            content = r.text,
            media_type='application/json',
            status_code = status.OK
            )
