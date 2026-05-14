from nicegui import app
from http import HTTPStatus as status
from fastapi.responses import JSONResponse
import requests
import logging

import settings as s

logger = logging.getLogger(__name__)

__base = "/api/v1/"

@app.get(__base + "id")
def id():
    logger.debug("id endpoint call")
    return "OMS API v1"

@app.get(__base + "weather")
def weather():
    logger.debug("weather endpoint call")
    url = s.q("weather", "url")
    try:
        logger.debug("Getting weather from {}".format(url))
        r = requests.get(url)
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
    return JSONResponse(
        content = r.json,
        status_code = status.OK
        )
