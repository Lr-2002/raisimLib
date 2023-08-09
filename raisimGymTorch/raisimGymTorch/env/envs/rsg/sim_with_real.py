from ruamel.yaml import YAML, dump, RoundTripDumper
from raisimGymTorch.env.RaisimGymVecEnv import RaisimGymVecEnv as VecEnv
from raisimGymTorch.helper.raisim_gym_helper import ConfigurationSaver, load_param, tensorboard_launcher, RaisimLogger
from raisimGymTorch.env.bin.rsg import NormalSampler
from raisimGymTorch.env.bin.rsg import RaisimGymEnv
from raisimGymTorch.env.RewardAnalyzer import RewardAnalyzer
import raisimGymTorch.algo.ppo.module as ppo_module
import raisimGymTorch.algo.ppo.ppo as PPO
from raisimGymTorch.env.deploy.angle_utils import transfer

import os
import math
import time
import torch.nn as nn
import numpy as np
import torch
import datetime
import argparse
# import pandas as pd
# from sine_generator import sine_generator
from unitree_deploy.sine_generator import  sine_generator
""""
todo 
    1. left-hand right-hand check
    2. raisim data check 

"""





# task specification
task_name = "sim2real"


"""
todo:
    1. make the model go to the same position
    2. pause check
    3. move 
        1. imu  info 
        2. gyroscope
        3. accelerator
        4. position
        5. velocity
    4. log
"""


# configuration
parser = argparse.ArgumentParser()
parser.add_argument('-m', '--mode', help='set mode either train or test', type=str, default='train')
parser.add_argument('-w', '--weight', help='pre-trained weight path', type=str, default='')
parser.add_argument('-u', '--update', help='update times', type=int, default=120)
parser.add_argument('-p', '--cfg_path', help='where to find the path', type=str, default=None)
# parser.add_argument('-b', '--load_best', help='load best file in last train', type=bool, default=False)
args = parser.parse_args()
mode = args.mode
weight_path = args.weight
cfg_path = args.cfg_path
# load_best = args.load_best
# check if gpu is available
device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')

# directories
task_path = os.path.dirname(os.path.realpath(__file__))
home_path = task_path + "/../../../../.."

# config
if cfg_path is not None:
    cfg = YAML().load(open(cfg_path, 'r'))
else:
    cfg = YAML().load(open(task_path + "/cfg.yaml", 'r'))
print(cfg['environment'])

# create environment from the configuration file
env = VecEnv(RaisimGymEnv(home_path + "/rsc", dump(cfg['environment'], Dumper=RoundTripDumper)))
env.seed(cfg['seed'])

dt = cfg['environment']['control_dt']
# shortcuts
ob_dim = env.num_obs
act_dim = env.num_acts
num_threads = cfg['environment']['num_threads']

# Training
n_steps = math.floor(cfg['environment']['max_time'] / cfg['environment']['control_dt'])
total_steps = n_steps * env.num_envs

avg_rewards = []

actor = ppo_module.Actor(ppo_module.MLP(cfg['architecture']['policy_net'], nn.LeakyReLU, ob_dim, act_dim),
                         ppo_module.MultivariateGaussianDiagonalCovariance(act_dim,
                                                                           env.num_envs,
                                                                           1.0,
                                                                           NormalSampler(act_dim),
                                                                           cfg['seed']),
                         device)
critic = ppo_module.Critic(ppo_module.MLP(cfg['architecture']['value_net'], nn.LeakyReLU, ob_dim, 1),
                           device)

saver = ConfigurationSaver(log_dir=home_path + "/raisimGymTorch/data/"+task_name,
                           save_items=[task_path + "/cfg.yaml", task_path + "/Environment.hpp"])
logger = RaisimLogger(saver.data_dir+"/train.log")

if mode =='train' or mode == 'retrain':
    tensorboard_launcher(saver.data_dir+"/..")  # press refresh (F5) after the first ppo update
num_envs = cfg['environment']['num_envs']
ppo = PPO.PPO(actor=actor,
              critic=critic,
              num_envs=cfg['environment']['num_envs'],
              num_transitions_per_env=n_steps,
              num_learning_epochs=4,
              gamma=0.996,
              lam=0.95,
              num_mini_batches=4,
              device=device,
              log_dir=saver.data_dir,
              shuffle_batch=False,
              learning_rate=cfg['environment']['learnning_rate']
              )

reward_analyzer = RewardAnalyzer(env, ppo.writer)
biggest_reward = 0
biggest_iter = 0
if mode == 'retrain':
    load_param(weight_path, env, actor, critic, ppo.optimizer, saver.data_dir)

check_done = lambda a, b: a + 1 if not b else 0

print = logger.info


total_update = args.update

schedule = cfg['environment']['schedule']
angle_rate = cfg['environment']['angle_rate']
act_rate = cfg['environment']['action_std'] # how many action generated use for work
act_rate = float(act_rate)



# NOTE : a1 init
moving_robot = True
if moving_robot:
    from robot_utils import *

    init_robot(dt)

    ori_posi = sine_generator(0, schedule, rate=angle_rate).tolist() # initial position
    # a1.kp = cfg['environment']['kp']
    # ori_posi = a1.position
    init_position(ori_posi)
    print(f"""
        now_posi: {a1.position},
        angle_rate : {angle_rate}
        schedule: {schedule}
    """)
    # input('Are you sure to go on?')





env.reset()
start = time.time()
onnx_flag = False
if onnx_flag:
    cnt_onnx = 0
    from raisimGymTorch.env.deploy import onnx_deploy
else:
    load_param(weight_path, env, actor, critic, ppo.optimizer, saver.data_dir)

envs_idx = [0] * num_envs
if moving_robot:
    real_idx = 0

for step in range(n_steps * 10):
    # time.sleep(0.01)
    obs = env.observe(False)

    print(f"""
    virtual_observation:
        {obs}
    """)

    if moving_robot:
        real_obs = a1.observe()
        print(f"""
        real_observation
            {real_obs}
        """)
    if onnx_flag:
        action = onnx_deploy.run_model(obs, cnt_onnx, 50)
        action = np.array(action)[0]
        cnt_onnx += 1
    else:
        action = ppo.act(obs)
        sine = sine_generator(envs_idx, schedule, angle_rate)
        action = transfer(action, sine, act_rate).astype(np.float32)
    if moving_robot:
        real_action = ppo.act(real_obs)
        sine = sine_generator(real_idx, schedule, angle_rate)
        real_action = transfer(real_action, sine, act_rate)
        # real_action = real_action
        a1.take_action(real_action.tolist())
        real_idx += 1

    reward, dones = env.step(action)
    envs_idx = list(map(check_done, envs_idx, dones))

print(f'biggest:{biggest_reward},rate = {biggest_iter}')


