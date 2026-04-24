from flask import Blueprint, jsonify
from http import HTTPStatus as status
import requests

api_v1_page = Blueprint("api_v1_page", __name__)

@api_v1_page.route("/api/v1/weather")
def weather():
    try:
        r = requests.get("http://allsky.fritz.box:8000")
    except:
        return jsonify(
                Error = "Could not send request"
                ), status.SERVICE_UNAVAILABLE


    if r.status_code != 200:
        return jsonify(
                Error = "Got HTTP code {} from endpoint".format(r.status_code)
                ), status.SERVICE_UNAVAILABLE


    return r.text, status.OK
